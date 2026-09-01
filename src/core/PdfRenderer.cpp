#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "PdfRenderer.h"
#include "PdfCoords.h"
#include "PageCache.h"
#include "PdfiumLock.h"
#include "../annotations/AnnotationManager.h"
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
#include <QCoreApplication>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

extern QMutex s_pdfiumMutex;

// ponytail: RAII wrapper that logs s_pdfiumMutex WAIT (>300ms) and HOLD (>300ms)
class TimedMutexLocker {
    QMutex&       m_m;
    const char*   m_label;
    QElapsedTimer m_hold;
public:
    TimedMutexLocker(QMutex& m, const char* label) : m_m(m), m_label(label) {
        QElapsedTimer w; w.start();
        m_m.lock();
        const qint64 waited = w.elapsed();
        m_hold.start();
        if (waited > 300) {
            const bool isMain = (QThread::currentThread() == QCoreApplication::instance()->thread());
            qDebug().noquote() << "[lockwait] ms=" << waited << "at" << m_label
                               << "main=" << (isMain ? 1 : 0);
        }
    }
    ~TimedMutexLocker() {
        const qint64 e = m_hold.elapsed();
        m_m.unlock();
        if (e > 300)
            qDebug().noquote() << "[lockhold] ms=" << e << "at" << m_label;
    }
    TimedMutexLocker(const TimedMutexLocker&) = delete;
    TimedMutexLocker& operator=(const TimedMutexLocker&) = delete;
};

#include "OwnAnnotHideGuard.h"

int ProgressivePauseCtx::sliceMsFromEnv() {
    static const int value = [] {
        int v = 50;
        bool ok = false;
        int parsed = qgetenv("TORREADER_SLICE_MS").toInt(&ok);
        if (ok && parsed > 0)
            v = parsed;
        qDebug().noquote() << "[perf] sliceMs=" << v;
        return v;
    }();
    return value;
}

std::atomic<int> PdfRenderer::s_renderCount{0};
std::atomic<qint64> PdfRenderer::s_globalCacheBytes{0};

qint64 PdfRenderer::maxCacheBytes() {
    static const qint64 budget = [] {
        const qint64 MB = 1024LL * 1024;
        const qint64 GB = 1024LL * MB;
        qint64 b = 0;
        quint64 ramBytes = 0;
#ifdef Q_OS_WIN
        MEMORYSTATUSEX ms;
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) ramBytes = ms.ullTotalPhys;
#endif
        if (ramBytes > 0) {
            b = static_cast<qint64>(ramBytes / 100) * 12;   // 12% RAM
            b = qBound(512LL * MB, b, 2LL * GB);            // kep [512 MB, 2 GB]
        } else {
            b = 1LL * GB;                                   // khong lay duoc RAM: 1 GB
        }
        qDebug().noquote() << "[cache] budget=" << (b / MB)
                           << "MB (RAM=" << (ramBytes / GB) << "GB)";
        return b;
    }();
    return budget;
}

// ── PageRenderTask ────────────────────────────────────────────────────────────

PageRenderTask::PageRenderTask(PdfRenderer* renderer, PdfDocument* pdfDoc, RenderRequest req,
                               QObject* receiver,
                               std::shared_ptr<QAtomicInt> genRef)
    : m_renderer(renderer), m_pdfDoc(pdfDoc), m_req(req)
    , m_genRef(std::move(genRef))
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

    QImage image;
    if (m_genRef->loadRelaxed() != m_req.generation) {
        qDebug() << "[perf] drop page=" << m_req.pageIndex << "reason=genMismatchPreLock thread=" << QThread::currentThreadId();
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

        OwnAnnotHideGuard _ah(page, m_req.hideOwnAnnots, true);

        double w = FPDF_GetPageWidth(page);
        double h = FPDF_GetPageHeight(page);
        if (m_pdfDoc) m_pdfDoc->updatePageSize(m_req.pageIndex, w, h);
        if (m_pdfDoc) m_pdfDoc->updatePageBoxOrigin(m_req.pageIndex, pdfBoxOrigin(page));

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
        int renderFlags = FPDF_RENDER_LIMITEDIMAGECACHE;
        if (m_req.renderAnnotations) renderFlags |= FPDF_ANNOT;
        FPDF_RenderPageBitmap(bmp, page, 0, 0, imgW, imgH, 0, renderFlags);
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
        // R1: cap doi acquirePage() — tra borrow (still inside s_pdfiumMutex).
        m_renderer->releasePage(m_req.pageIndex);
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

// 🔴 CONG TAC A/B 2026-09-01: TORREADER_ALLOW_PARTIAL=1 tra lai hanh vi CU (phat ca anh ve do).
// Dung de tra loi dut khoat: bo va "chan anh ve do" co phai thu lam mat markup khong.
// Doi mot bien moi truong re hon nhieu so voi dung lai hai ban exe roi doan.
// 🔴🔴 DAO MAC DINH 2026-09-01: owner bao MAT MARKUP o CA Single lan Continuous.
// Viec chan anh ve do (them 31/08) la thay doi DUY NHAT trong ngay cham vao ma DUNG CHUNG
// ca hai che do — va trieu chung cung la trieu chung dung chung. Chua chung minh duoc nhan qua,
// nhung markup dung > anh khong bi thieu net: tra ve hanh vi cu, bat lai bang
// TORREADER_BLOCK_PARTIAL=1 khi can doi chung.
// 🔴🔴 CHOT LAI 2026-09-01 — nay CO BANG CHUNG SO, khong con la phong doan.
// Do tren tep that, trang 3 @75%:  con dem cu = 60.800 net  |  don sach dem = 319.866 net.
// Gap 5,3 lan ⇒ dem dia dang giu mot ANH HONG gan nhu rong va app doc lai no moi lan mo.
// Nguon cua anh hong: luot render bi CAT GIUA CHUNG van duoc phat ra nhu anh hoan chinh roi
// GHI VAO DEM. Chot doc chi kiem KICH THUOC nen anh du kich thuoc ma thieu net van lot.
// ⇒ Chua ve xong thi KHONG phat ra, do do cung khong bao gio vao dem.
static bool allowPartialEnv() {
    static const bool on = !qEnvironmentVariableIsEmpty("TORREADER_ALLOW_PARTIAL");
    return on;
}

// ── ProgressiveRenderTask ─────────────────────────────────────────────────────

ProgressiveRenderTask::ProgressiveRenderTask(PdfRenderer* renderer, PdfDocument* pdfDoc, RenderRequest req,
                                             QObject* receiver, std::shared_ptr<QAtomicInt> genRef)
    : m_renderer(renderer), m_pdfDoc(pdfDoc), m_req(req)
    , m_genRef(std::move(genRef))
{
    setAutoDelete(true);
    connect(this, &ProgressiveRenderTask::finished, receiver,
            [](int, QImage){}, Qt::QueuedConnection);
    connect(this, &ProgressiveRenderTask::pagePartial, receiver,
            [](int, double, QImage){}, Qt::QueuedConnection);
}

ProgressiveRenderTask::~ProgressiveRenderTask() {
    if (m_poolHandle) {
        m_renderer->returnPoolHandle(m_poolHandle);
        m_poolHandle = nullptr;
    }
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
    // 🔴 LOG-ONLY 2026-09-01: hai co quyet dinh chu thich co duoc ve vao anh khong.
    qDebug().noquote() << "[annotflag] page=" << m_req.pageIndex
                       << "veChuThich=" << m_req.renderAnnotations
                       << "giauMarkupCuaTa=" << m_req.hideOwnAnnots;

    // VIỆC B: Thử mượn handle từ pool để render mà không cần s_pdfiumMutex
    // 🔴 2026-09-01: TRU trang vua co chu thich moi — ban sao trong pool khong chua chung.
    // ⚠️ 2026-09-01: trang co chu thich MOI phai ve tu TAI LIEU CHINH (ban sao chua co no).
    // Doi lai, duong ve tien-dan NHA KHOA giua cac lat nen co khe hoi cho luong giao dien sua
    // tai lieu ⇒ nguon cua sap app (PDFium 0x80000003 / heap 0xc0000374 trong Event Log).
    // Chua sua duoc trong phien nay — huong dung: giu khoa suot mot luot ve cho trang "ban",
    // hoac nap lai ban sao theo kieu hoan lai (khong dong khi con nguoi muon).
    if (m_renderer->pageAnnotDirty(m_req.pageIndex)) {
        m_poolHandle = nullptr;
    } else {
        m_poolHandle = m_renderer->borrowPoolHandle();
    }

    std::unique_ptr<OwnAnnotHideGuard> _ahGuard;
    QElapsedTimer renderTimer;
    renderTimer.start();

    // ── Step 1: FPDF_RenderPageBitmap_Start ───────────────────────────────────
    int startStatus = FPDF_RENDER_READY;

    // VIỆC B: Nếu có pool handle, KHÔNG cần s_pdfiumMutex cho render (song song).
    // Chỉ cần khoá cho PageCache operations (acquirePage/releasePage sẽ giữ s_pdfiumMutex).
    if (m_poolHandle) {
        // Đường pool handle: không cần khoá cho render
        qDebug() << "[song bang] trang=" << m_req.pageIndex << "dung pool handle";

        // FPDF_LoadPage từ pool handle (KHÔNG cần khoá chung)
        FPDF_PAGE poolPage = FPDF_LoadPage(m_poolHandle, m_req.pageIndex);
        if (!poolPage) {
            qDebug() << "[perf] drop page=" << m_req.pageIndex << "reason=poolLoadPageFailed";
            m_renderer->returnPoolHandle(m_poolHandle);
            emit finished(m_req.pageIndex, QImage());
            return;
        }

        if (m_req.hideOwnAnnots)
            _ahGuard = std::make_unique<OwnAnnotHideGuard>(poolPage, true, true);

        double w = FPDF_GetPageWidth(poolPage);
        double h = FPDF_GetPageHeight(poolPage);
        if (m_pdfDoc) m_pdfDoc->updatePageSize(m_req.pageIndex, w, h);
        if (m_pdfDoc) m_pdfDoc->updatePageBoxOrigin(m_req.pageIndex, pdfBoxOrigin(poolPage));

        double longSide = qMax(w, h);
        double maxPx;
        if (m_req.fullQuality && m_req.useZoomScale) {
            maxPx = PdfRenderer::kFullRenderMaxPx;
            qDebug().noquote() << "[dem] page=" << m_req.pageIndex << "render DAY DU"
                               << int(maxPx) << "px (hien thi thu nho ve"
                               << PdfRenderer::contDpi() << "DPI)";
        } else if (m_req.fullQuality) {
            maxPx = PdfRenderer::kFullRenderMaxPx;
        } else {
            maxPx = qMin(m_req.scaleFactor * longSide, PdfRenderer::kThumbMaxPx);
        }
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
        { int rflags = FPDF_RENDER_LIMITEDIMAGECACHE;
          if (m_req.renderAnnotations) rflags |= FPDF_ANNOT;
        startStatus = FPDF_RenderPageBitmap_Start(m_bmp, poolPage, 0, 0,
                                                   m_bmpW, m_bmpH, 0,
                                                   rflags,
                                                   &pause); }

        m_renderStatus = startStatus;
        m_fpdfPage = poolPage;  // Lưu page để dùng trong loop Continue

        // Emit first partial after Start
        if (m_renderStatus == FPDF_RENDER_TOBECONTINUED || m_renderStatus == FPDF_RENDER_DONE) {
            QImage partial = m_image.copy();
            emit pagePartial(m_req.pageIndex, scaleFactor(), partial);
            qDebug() << "[perf] slice ms=" << renderTimer.elapsed() << "page=" << m_req.pageIndex;
        }

        // ── Loop Continue với pool handle (KHÔNG cần khoá) ──
        m_lastEmitTimer.start();
        while (m_renderStatus == FPDF_RENDER_TOBECONTINUED) {
            if (m_genRef->loadRelaxed() != m_req.generation) {
                qDebug() << "[perf] drop page=" << m_req.pageIndex << "reason=genMismatchMid progressive";
                break;
            }

            ProgressivePauseCtx pctx2;
            pctx2.timer.start();
            IFSDK_PAUSE pause2;
            pause2.version = 1;
            pause2.NeedToPauseNow = ProgressiveNeedToPauseNow;
            pause2.user = &pctx2;

            m_renderStatus = FPDF_RenderPage_Continue(poolPage, &pause2);

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

        // Cleanup với pool handle
        {
            if (m_renderStatus == FPDF_RENDER_TOBECONTINUED || m_renderStatus == FPDF_RENDER_DONE)
                FPDF_RenderPage_Close(poolPage);
            if (m_bmp) { FPDFBitmap_Destroy(m_bmp); m_bmp = nullptr; }

            _ahGuard.reset();

            int objCount = FPDFPage_CountObjects(poolPage);
            emit pageObjectCount(m_req.pageIndex, objCount);

            FPDF_ClosePage(poolPage);
        }

        m_renderer->returnPoolHandle(m_poolHandle);
        m_poolHandle = nullptr;

        // 🔴🔴 SUA 2026-08-31 — GOC CUA "trang quai vat luot qua chi thay may net".
        // Vong lap tren THOAT SOM khi the he doi (genMismatchMid) — luc do m_renderStatus van la
        // TOBECONTINUED, KHONG phai FAILED. Ban cu chi chan FAILED nen anh VE DO roi thang xuong
        // `emit finished(m_image)` va duoc doi xu nhu ANH DAY DU: hien len man hinh VA ghi vao
        // bo dem CA bo nho LAN dia. Chot bo dem chi do CANH DAI nen anh thieu net van lot.
        // ⇒ Mot trang bi cat giua chung la HONG VINH VIEN tren dia; lan sau mo lai van thieu net.
        // Khung ten nam o goc, thuoc phan ve SAU CUNG, nen la thu mat dau tien — dung trieu chung.
        // Owner chot: "trang luot qua phai TRON VEN, du doi tuong, MO cung duoc".
        // ⇒ Chua ve xong thi coi nhu KHONG CO ANH: tra rong, de duong ve tut xuong thumbnail
        // (day du, mo) va de co che dat lai lenh render chay.
        if (m_renderStatus != FPDF_RENDER_DONE && !allowPartialEnv()) {
            qDebug() << "[perf] render CHUA XONG page=" << m_req.pageIndex
                     << "status=" << m_renderStatus
                     << "— vut anh ve do, KHONG ghi bo dem";
            emit finished(m_req.pageIndex, QImage());
            return;
        }

        qDebug() << "[perf] render done page=" << m_req.pageIndex
                 << "ms=" << renderTimer.elapsed()
                 << "thread=" << QThread::currentThreadId();
        emit finished(m_req.pageIndex, std::move(m_image));
        return;
    }

    // VIỆC B: Pool rỗi — dùng đường cũ (s_pdfiumMutex)
    //
    // 🔴🔴 VA CHONG SAP 2026-09-01.
    // Duong nay NHA KHOA giua cac lat ve. Voi trang vua sua chu thich, no phai ve tu TAI LIEU
    // CHINH (ban sao chua co chu thich) — nen trong khe ho do luong giao dien co the sua chinh
    // tai lieu dang duoc ve dang do. Ket qua: PDFium tu bung loi (0x80000003) roi hong heap
    // (0xc0000374) — da thay trong Event Log cua may owner.
    // ⇒ Voi trang "ban" ma KHONG NANG: giu khoa SUOT mot luot ve, khong nha giua chung.
    // ⚠️ Chi lam cho trang nhe. Trang quai vat ma giu khoa ca luot thi giao dien DUNG HINH —
    //    dieu owner cam tuyet doi. Trang nang giu nguyen hanh vi cu (van co nguy co, nhung hiem
    //    khi ai them chu thich vao trang quai vat, va dung hinh la loi nang hon).
    const int soObj = m_renderer->pageObjectCount(m_req.pageIndex);
    // ⚠️ soObj == 0 nghia la CHUA BIET (chua render lan nao) — ngay sau khi sua chu thich thi
    // thuong roi vao truong hop nay. Coi "chua biet" la NHE: da do, dieu kien cu lam ban va
    // KHONG BAO GIO kich hoat (0 lan). Chi bo qua khi BIET CHAC la trang nang.
    const bool bietChacNang = (soObj >= 200000);
    const bool giuKhoaSuot = m_renderer->pageAnnotDirty(m_req.pageIndex) && !bietChacNang;
    std::unique_ptr<TimedMutexLocker> khoaSuot;
    if (giuKhoaSuot) {
        qDebug().noquote() << "[lock] page=" << m_req.pageIndex
                           << "GIU KHOA suot luot ve (trang vua sua chu thich, objects=" << soObj << ")";
        khoaSuot = std::make_unique<TimedMutexLocker>(s_pdfiumMutex, "ProgressiveRenderTask::GiuSuot");
    }
    {
        std::unique_ptr<TimedMutexLocker> lockStart;
        if (!giuKhoaSuot)
            lockStart = std::make_unique<TimedMutexLocker>(s_pdfiumMutex, "ProgressiveRenderTask::Start");
        if (m_genRef->loadRelaxed() != m_req.generation) {
            qDebug() << "[perf] drop page=" << m_req.pageIndex << "reason=genMismatchPreStart";
            emit finished(m_req.pageIndex, QImage()); return;
        }

        m_fpdfPage = m_renderer->acquirePage(m_req.pageIndex);
        if (!m_fpdfPage) { emit finished(m_req.pageIndex, QImage()); return; }

        if (m_req.hideOwnAnnots)
            _ahGuard = std::make_unique<OwnAnnotHideGuard>(m_fpdfPage, true, true);

        double w = FPDF_GetPageWidth(m_fpdfPage);
        double h = FPDF_GetPageHeight(m_fpdfPage);
        if (m_pdfDoc) m_pdfDoc->updatePageSize(m_req.pageIndex, w, h);
        if (m_pdfDoc) m_pdfDoc->updatePageBoxOrigin(m_req.pageIndex, pdfBoxOrigin(m_fpdfPage));

        double longSide = qMax(w, h);
        double maxPx;
        // ── DO PHAN GIAI CUA CONTINUOUS: SIET THEO DPI TUNG TRANG, KHONG DUNG TRAN PIXEL ──
        // (owner chot 2026-08-31). 72pt = 1 inch, nen so pixel do CHINH KHO GIAY quyet dinh.
        //
        // Vi sao bo tran 4000px: tran ap cung mot so pixel cho moi kho giay nen chat luong THAT
        // lai lech nhau — A0 (2381pt) chi duoc 121 DPI (tho), A4 (842pt) duoc toi 342 DPI (thua).
        // Siet theo DPI thi A0 net hon VA A4 nhe di, duoc ca hai dau.
        //
        // Vi sao 150 chu khong 300 (da DO 2026-08-31 tren trang A0 2,54 trieu doi tuong):
        //   150 DPI -> 4960 px, 2071 ms/trang, cache 199 MB, 3/3 trang XONG
        //   300 DPI -> 9921 px, 266 MB MOT trang -> "render FAILED page=2" + "cont drop
        //              reason=nullResult" khi nhieu trang render cung luc => TRANG TRONG.
        // ⇒ Day chinh la mot trong nhung nguon "trang trang/den": day do phan giai len cao
        //   lam cap phat anh that bai, va no hong IM LANG.
        // Doi theo luot: TORREADER_CONT_DPI=<n>.
        if (m_req.fullQuality && m_req.useZoomScale) {
            // 🔴 THIET KE 2026-08-31 (owner chot): MOT o dem duy nhat, CHAT LUONG DAY DU,
            // Single va Continuous DUNG CHUNG. Continuous khong ghi anh thap vao o Full nua —
            // truoc do no ghi 2480px (75 DPI) vao chinh o Single doc, lam Single mo theo.
            //
            // Vi sao render day du ma van nhanh: DA DO — tang 6,15 lan so pixel chi lam thoi gian
            // tang chua toi gap doi (khau to pixel chi ~10-20% chi phi; 80% la PDFium doc noi dung).
            // Nen render day du MOT LAN roi thu nho de hien la re hon render lai moi muc zoom.
            // Hien thi: ContinuousView tu `scaledToWidth(needW*1.15)` ve dung DPI dang xem.
            maxPx = PdfRenderer::kFullRenderMaxPx;
            qDebug().noquote() << "[dem] page=" << m_req.pageIndex << "render DAY DU"
                               << int(maxPx) << "px (hien thi thu nho ve"
                               << PdfRenderer::contDpi() << "DPI)";
        } else if (m_req.fullQuality) {
            maxPx = PdfRenderer::kFullRenderMaxPx;
        } else {
            maxPx = qMin(m_req.scaleFactor * longSide, PdfRenderer::kThumbMaxPx);
        }
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
        { int rflags = FPDF_RENDER_LIMITEDIMAGECACHE;
          if (m_req.renderAnnotations) rflags |= FPDF_ANNOT;
        m_renderStatus = FPDF_RenderPageBitmap_Start(m_bmp, m_fpdfPage, 0, 0,
                                                       m_bmpW, m_bmpH, 0,
                                                       rflags,
                                                       &pause); }
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
            std::unique_ptr<TimedMutexLocker> lockCont;
            if (!giuKhoaSuot)
                lockCont = std::make_unique<TimedMutexLocker>(s_pdfiumMutex, "ProgressiveRenderTask::Continue");
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

    // 🔴 GO BO 2026-09-01: da thu ve lop form (FPDF_FFLDraw) ngay tai day de tra lai nen cho
    // comment. HONG NANG: goi PDFium khi luot render tien-dan CHUA DONG va KHONG giu khoa chung
    // ⇒ pha luon anh dang ve (owner: "re-render duoc vai net, con lai trang tron", va trang moi
    //   tao khong bao gio hien). Neu lam lai: phai ve form SAU khi FPDF_RenderPage_Close va
    //   PHAI nam trong TimedMutexLocker, hoac ve o mot luot rieng.
    // ── Step 3: Close / finalise ──────────────────────────────────────────────
    bool cancelled = (m_genRef->loadRelaxed() != m_req.generation);

    // Emit one final partial if Done
    if (m_renderStatus == FPDF_RENDER_DONE && !cancelled) {
        QImage finalPartial = m_image.copy();
        emit pagePartial(m_req.pageIndex, scaleFactor(), finalPartial);
    }

    {
        std::unique_ptr<TimedMutexLocker> lockClose;
        if (!giuKhoaSuot)
            lockClose = std::make_unique<TimedMutexLocker>(s_pdfiumMutex, "ProgressiveRenderTask::Close");
        if (m_renderStatus == FPDF_RENDER_TOBECONTINUED || m_renderStatus == FPDF_RENDER_DONE)
            FPDF_RenderPage_Close(m_fpdfPage);
        if (m_bmp) { FPDFBitmap_Destroy(m_bmp); m_bmp = nullptr; }

        _ahGuard.reset();  // unhide our annots before closing page

        int objCount = 0;
        if (m_fpdfPage) {
            objCount = FPDFPage_CountObjects(m_fpdfPage);
            emit pageObjectCount(m_req.pageIndex, objCount);
        }
        // R1: cap doi acquirePage() — moi duong thoat sau acquire deu qua block nay
        // (ke ca genMismatchMid -> break). Tra borrow khi xong (con giu s_pdfiumMutex).
        m_renderer->releasePage(m_req.pageIndex);
    }

    if (cancelled) {
        qDebug() << "[perf] drop page=" << m_req.pageIndex << "reason=genMismatchPost progressive";
        emit finished(m_req.pageIndex, QImage());
        return;
    }

    // 🔴🔴 SUA 2026-08-31 — GOC CUA "trang quai vat luot qua chi thay may net".
    // Vong lap tren THOAT SOM khi the he doi (genMismatchMid) — luc do m_renderStatus van la
    // TOBECONTINUED, KHONG phai FAILED. Ban cu chi chan FAILED nen anh VE DO roi thang xuong
    // `emit finished(m_image)` va duoc doi xu nhu ANH DAY DU: hien len man hinh VA ghi vao
    // bo dem CA bo nho LAN dia. Chot bo dem chi do CANH DAI nen anh thieu net van lot.
    // ⇒ Mot trang bi cat giua chung la HONG VINH VIEN tren dia; lan sau mo lai van thieu net.
    // Khung ten nam o goc, thuoc phan ve SAU CUNG, nen la thu mat dau tien — dung trieu chung.
    // Owner chot: "trang luot qua phai TRON VEN, du doi tuong, MO cung duoc".
    // ⇒ Chua ve xong thi coi nhu KHONG CO ANH: tra rong, de duong ve tut xuong thumbnail
    // (day du, mo) va de co che dat lai lenh render chay.
    if (m_renderStatus != FPDF_RENDER_DONE && !allowPartialEnv()) {
        qDebug() << "[perf] render CHUA XONG page=" << m_req.pageIndex
         << "status=" << m_renderStatus
         << "— vut anh ve do, KHONG ghi bo dem";
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
                                   std::shared_ptr<QAtomicInt> genRef,
                                   bool renderAnnotations,
                                   bool hideOwnAnnots)
    : m_renderer(renderer), m_pdfDoc(pdfDoc)
    , m_pageIndex(pageIndex), m_scale(scale), m_regionPx(regionPx)
    , m_genRef(std::move(genRef))
    , m_reqGen(m_genRef ? m_genRef->loadRelaxed() : 0)
    , m_renderAnnotations(renderAnnotations)
    , m_hideOwnAnnots(hideOwnAnnots)
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

    // ── Step 1: Start — lock, load page, create bitmap, FPDF_RenderPageBitmap_Start ──
    {
        TimedMutexLocker lock(s_pdfiumMutex, "RegionRenderTask::Start");
        if (m_genRef->loadRelaxed() != m_reqGen) {
            emit finished(m_pageIndex, m_scale, m_regionPx, QImage()); return;
        }

        m_fpdfPage = m_renderer->acquirePage(m_pageIndex);
        if (!m_fpdfPage) {
            emit finished(m_pageIndex, m_scale, m_regionPx, QImage()); return;
        }

        OwnAnnotHideGuard _ah(m_fpdfPage, m_hideOwnAnnots, true);

        double wPt = FPDF_GetPageWidth(m_fpdfPage);
        double hPt = FPDF_GetPageHeight(m_fpdfPage);
        double fullW = qMax(1.0, wPt * m_scale);
        double fullH = qMax(1.0, hPt * m_scale);
        m_bmpW = rw;
        m_bmpH = rh;

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
        { int rflags = FPDF_RENDER_LIMITEDIMAGECACHE;
          if (m_renderAnnotations) rflags |= FPDF_ANNOT;
          m_renderStatus = FPDF_RenderPageBitmap_Start(m_bmp, m_fpdfPage,
                                                        -m_regionPx.x(), -m_regionPx.y(),
                                                        static_cast<int>(fullW),
                                                        static_cast<int>(fullH),
                                                        0, rflags, &pause); }
        // Mutex unlocked here — other threads can use PDFium between slices
    }

    // ── Step 2: Continue loop — lock per slice, unlock between ──────────────────
    while (m_renderStatus == FPDF_RENDER_TOBECONTINUED) {
        if (m_genRef->loadRelaxed() != m_reqGen) {
            qDebug() << "[perf] region HUY giua chung page=" << m_pageIndex;
            break;
        }

        {
            TimedMutexLocker lock(s_pdfiumMutex, "RegionRenderTask::Continue");
            ProgressivePauseCtx pctx;
            pctx.timer.start();
            IFSDK_PAUSE pause;
            pause.version = 1;
            pause.NeedToPauseNow = ProgressiveNeedToPauseNow;
            pause.user = &pctx;

            m_renderStatus = FPDF_RenderPage_Continue(m_fpdfPage, &pause);
            // Mutex unlocked here — other threads can use PDFium between slices
        }
    }

    // ── Step 3: Close — lock, cleanup resources ─────────────────────────────────
    {
        TimedMutexLocker lock(s_pdfiumMutex, "RegionRenderTask::Close");
        if (m_renderStatus == FPDF_RENDER_TOBECONTINUED || m_renderStatus == FPDF_RENDER_DONE)
            FPDF_RenderPage_Close(m_fpdfPage);
        if (m_bmp) { FPDFBitmap_Destroy(m_bmp); m_bmp = nullptr; }
        // R1: cap doi acquirePage() — moi duong thoat sau acquire deu qua block nay.
        m_renderer->releasePage(m_pageIndex);
    }

    if (m_renderStatus == FPDF_RENDER_FAILED) {
        qDebug() << "[perf] region render FAILED page=" << m_pageIndex;
        emit finished(m_pageIndex, m_scale, m_regionPx, QImage());
        return;
    }

    if (m_genRef->loadRelaxed() != m_reqGen) {
        qDebug() << "[perf] region HUY sau render page=" << m_pageIndex;
        emit finished(m_pageIndex, m_scale, m_regionPx, QImage());
        return;
    }

    emit finished(m_pageIndex, m_scale, m_regionPx, std::move(m_image));
}

// ── TileBatchRenderTask ────────────────────────────────────────────────────────

TileBatchRenderTask::TileBatchRenderTask(PdfRenderer* renderer, PdfDocument* pdfDoc,
                                         int pageIndex, double scale,
                                         QVector<QPoint> tiles,
                                         QObject* receiver,
                                         std::shared_ptr<QAtomicInt> genRef,
                                         bool renderAnnotations,
                                         bool hideOwnAnnots)
    : m_renderer(renderer), m_pdfDoc(pdfDoc)
    , m_pageIndex(pageIndex), m_scale(scale)
    , m_tiles(std::move(tiles))
    , m_genRef(std::move(genRef))
    , m_reqGen(m_genRef ? m_genRef->loadRelaxed() : 0)
    , m_renderAnnotations(renderAnnotations)
    , m_hideOwnAnnots(hideOwnAnnots)
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

        OwnAnnotHideGuard _ah(page, m_hideOwnAnnots, true);

        double wPt = FPDF_GetPageWidth(page);
        double hPt = FPDF_GetPageHeight(page);
        if (m_pdfDoc) m_pdfDoc->updatePageSize(m_pageIndex, wPt, hPt);
        if (m_pdfDoc) m_pdfDoc->updatePageBoxOrigin(m_pageIndex, pdfBoxOrigin(page));

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
                // R1: cap doi acquirePage() — huy giua chung van phai tra borrow.
                m_renderer->releasePage(m_pageIndex);
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
            { int rflags = 0;
              if (m_renderAnnotations) rflags |= FPDF_ANNOT;
            FPDF_RenderPageBitmap(bmp, page,
                                  -col * kTileSize, -row * kTileSize,
                                  fullW, fullH, 0, rflags); }
            if (form) {
                FPDF_FFLDraw(form, bmp, page,
                             -col * kTileSize, -row * kTileSize,
                             fullW, fullH, 0, FPDF_ANNOT);
            }
            FPDFBitmap_Destroy(bmp);

            emit tileDone(m_pageIndex, m_scale, col, row, std::move(image));
        }

        if (form) { FORM_OnBeforeClosePage(page, form); FPDFDOC_ExitFormFillEnvironment(form); }
        // R1: cap doi acquirePage() — tra borrow khi ve xong toan bo tile.
        m_renderer->releasePage(m_pageIndex);
    }
}

// ── Page handle ────────────────────────────────────────────────────────────────

FPDF_PAGE PdfRenderer::acquirePage(int pageIndex) {
    if (!m_doc || !m_doc->raw()) return nullptr;
    // R1: moi truy cap FPDF_PAGE di qua PageCache — NGUON SU THAT DUY NHAT.
    // GIA DINH caller DA giu s_pdfiumMutex (dung nhu luat PageCache.h).
    return PageCache::acquire(m_doc->raw(), pageIndex);
}

void PdfRenderer::releasePage(int pageIndex) {
    // GIA DINH caller giu s_pdfiumMutex (dung nhu acquirePage). Cap doi bat buoc.
    if (m_doc) PageCache::release(m_doc->raw(), pageIndex);
}

// ── Heavy page detection ───────────────────────────────────────────────────────

qint64 PdfRenderer::globalCacheBytes() { return s_globalCacheBytes.load(); }

qint64 PdfRenderer::tabCacheBytes() const {
    qint64 t = 0;
    for (auto it = m_cache.constBegin(); it != m_cache.constEnd(); ++it)
        t += it.value().sizeInBytes();
    return t;
}

int PdfRenderer::contDpi() {
    static const int v = []{
        const int e = qEnvironmentVariableIntValue("TORREADER_CONT_DPI");
        return e > 0 ? e : 150;
    }();
    return v;
}

double PdfRenderer::contTargetLongPx(double longSidePt) {
    return qMax(1.0, longSidePt) / 72.0 * double(contDpi());
}

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
{
    const int cpus = qMax(1, QThread::idealThreadCount());
    m_thumbPool.setMaxThreadCount(qMax(2, cpus / 2));
    m_thumbPool.setExpiryTimeout(-1);
    m_mainPool.setMaxThreadCount(4);
    m_mainPool.setExpiryTimeout(-1);
}

PdfRenderer::~PdfRenderer() {
    closeHandlePool();
    s_globalCacheBytes.fetch_sub(m_cacheBytes);
    m_cacheBytes = 0;
    m_generation->fetchAndAddOrdered(999);
    m_thumbPool.waitForDone();
    m_mainPool.waitForDone();
    if (s_renderCount.load() > 0)
        qDebug() << "[Renderer] total FPDF_RenderPageBitmap calls:" << s_renderCount.load();
}

void PdfRenderer::setDocument(PdfDocument* doc) {
    cancelPending();
    m_mainPool.waitForDone();
    m_thumbPool.waitForDone();
    closeHandlePool();
    m_doc = doc;
    clearCache();
    m_pageObjectCount.clear();
    m_pageAnnotRender.clear();
    m_pageAnnotOverlay.clear();
    initHandlePool();
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
    // VIỆC 2: huy theo trang
    for (auto& kv : m_contPageGen) kv->fetchAndAddOrdered(1);
    m_contPageGen.clear();
    m_continuousQueue.clear();
}

void PdfRenderer::cancelContinuous() {
    // Huy SACH: bump het per-page gen + xoa hang doi. Dung cho zoom change.
    m_continuousQueue.clear();
    m_continuousGen->fetchAndAddOrdered(1);
    for (auto& kv : m_contPageGen) kv->fetchAndAddOrdered(1);
    m_contPageGen.clear();
}

// VIỆC 2 (SPEC_SMOOTH_123 31/08): huy dung MOT trang. Chi huy trang khong con
// nhin thay, tra khe ve. ContinuousView goi trong requestVisiblePages thay vi
// cancelContinuous() huy sach.
void PdfRenderer::cancelContinuousPage(int page) {
    // Bump per-page gen: in-flight task of this page drops at next slice.
    auto it = m_contPageGen.find(page);
    if (it != m_contPageGen.end()) it.value()->fetchAndAddOrdered(1);
    // Remove from queue.
    for (int i = m_continuousQueue.size() - 1; i >= 0; --i)
        if (m_continuousQueue[i].page == page)
            m_continuousQueue.removeAt(i);
}

bool PdfRenderer::isContinuousPageActive(int page) const {
    // Check if page is in queue or has a per-page gen entry (rendering or queued).
    if (m_contPageGen.contains(page)) return true;
    for (const auto& j : m_continuousQueue)
        if (j.page == page) return true;
    return false;
}

void PdfRenderer::cancelContinuousQueueOnly() {
    // Chi bo yeu cau CHUA chay. KHONG dung m_continuousGen => render dang chay van ve xong
    // va van phat continuousPageReady. Dung khi trang cu con nhin thay.
    m_continuousQueue.clear();
}

// R2 (SPEC_PERF_HEAVYPAGE): chi huy render full-quality dang chay. Lenh render do phan giai
// THAP (thumbnail / preload qua m_generation, continuous qua m_continuousGen) van chay binh
// thuong — day la cai "anh mo tam luc lat trang" phai giu.
void PdfRenderer::cancelFullQuality() {
    m_fullRenderGen->fetchAndAddOrdered(1);
    m_fullRenderRunning = false;
}

void PdfRenderer::setSuppressFullQuality(int pageIndex, bool suppress) {
    if (suppress) m_suppressedFullQuality.insert(pageIndex);
    else          m_suppressedFullQuality.remove(pageIndex);
}

// ── Handle Pool Management (VIỆC B) ───────────────────────────────────────────

void PdfRenderer::reloadHandlePool() {
    qDebug().noquote() << "[pool] nap lai ban sao tai lieu (sau khi sua chu thich)";
    closeHandlePool();
    initHandlePool();
}

void PdfRenderer::initHandlePool() {
    if (!m_doc || !m_doc->isOpen() || !m_doc->hasMmap()) {
        qDebug() << "[song song] pool SKIP — no mmap available";
        return;
    }
    QMutexLocker lock(&m_handlePoolMutex);
    m_handlePool.clear();

    // Mở kPoolSize handle bổ sung từ cùng buffer memory-mapped
    for (int i = 0; i < kPoolSize; ++i) {
        BoundedPdfiumLock plock(__FILE__, __LINE__);
        FPDF_DOCUMENT poolDoc = FPDF_LoadMemDocument(
            m_doc->mmapData(),
            static_cast<int>(m_doc->mmapSize()),
            m_doc->password()
        ); if (poolDoc) g_pdfiumPoolOpen.fetchAndAddOrdered(1);
        if (poolDoc) {
            m_handlePool.push_back({poolDoc, false});
            qDebug() << "[song song] pool INIT slot=" << i;
        } else {
            qDebug() << "[song song] pool FAIL open slot=" << i;
        }
    }
}

void PdfRenderer::closeHandlePool() {
    QMutexLocker lock(&m_handlePoolMutex);
    for (auto& slot : m_handlePool) {
        if (slot.doc) {
            BoundedPdfiumLock plock(__FILE__, __LINE__);
            FPDF_CloseDocument(slot.doc); g_pdfiumPoolClose.fetchAndAddOrdered(1);
            slot.doc = nullptr;
        }
    }
    m_handlePool.clear();
}

bool PdfRenderer::hasPoolHandles() const {
    QMutexLocker lk(&m_handlePoolMutex);
    return !m_handlePool.empty();
}

FPDF_DOCUMENT PdfRenderer::borrowPoolHandle() {
    QMutexLocker lock(&m_handlePoolMutex);
    // Tìm handle rỗi đầu tiên
    for (auto& slot : m_handlePool) {
        if (slot.doc && !slot.inUse) {
            slot.inUse = true;
            qDebug() << "[song song] muon handle — available";
            return slot.doc;
        }
    }
    // Pool hết handle rỗi — return nullptr để dùng đường cũ
    qDebug() << "[song song] muon handle — pool empty, fallback to main doc";
    return nullptr;
}

void PdfRenderer::returnPoolHandle(FPDF_DOCUMENT handle) {
    if (!handle) return;
    QMutexLocker lock(&m_handlePoolMutex);
    for (auto& slot : m_handlePool) {
        if (slot.doc == handle) {
            slot.inUse = false;
            qDebug() << "[song song] tra handle";
            return;
        }
    }
}

// VIỆC THUMBNAIL SONG PARALLEL: public wrappers để ThumbnailWorker dùng
FPDF_DOCUMENT PdfRenderer::borrowPoolHandleForThumbnail() {
    {   // Chan truoc theo han muc: thumbnail khong duoc chiem qua kThumbHandleQuota handle,
        // de render TRANG luon con it nhat (kPoolSize - kThumbHandleQuota) handle ma dung.
        QMutexLocker lock(&m_handlePoolMutex);
        if (m_thumbHandlesInUse >= kThumbHandleQuota) {
            qDebug() << "[thumb quota] du" << m_thumbHandlesInUse << "/" << kThumbHandleQuota
                     << "— cho luot sau";
            return nullptr;   // di duong cu (s_pdfiumMutex), khong cuop cho cua render trang
        }
    }
    FPDF_DOCUMENT h = borrowPoolHandle();
    if (h) {
        QMutexLocker lock(&m_handlePoolMutex);
        ++m_thumbHandlesInUse;
    }
    return h;
}

void PdfRenderer::returnPoolHandleForThumbnail(FPDF_DOCUMENT handle) {
    if (handle) {
        QMutexLocker lock(&m_handlePoolMutex);
        if (m_thumbHandlesInUse > 0) --m_thumbHandlesInUse;
    }
    returnPoolHandle(handle);
}

void PdfRenderer::clearStalePending() {
    m_pendingRequests.clear();
}

void PdfRenderer::cacheInsert(int pageIndex, const QImage& img) {
    auto it = m_cache.find(pageIndex);
    if (it != m_cache.end()) {
        const qint64 oldSz = static_cast<qint64>(it.value().sizeInBytes());
        m_cacheBytes -= oldSz;
        s_globalCacheBytes.fetch_sub(oldSz);
    }
    const qint64 sz = static_cast<qint64>(img.sizeInBytes());
    m_cacheBytes += sz;
    s_globalCacheBytes.fetch_add(sz);
    m_cache.insert(pageIndex, img);
}

// Single O(n log n) pass: sort entries by distance from current page, evict farthest first.
void PdfRenderer::evictCache() {
    const qint64 budget = maxCacheBytes();
    if (s_globalCacheBytes.load() <= budget) return;
    using KV = QPair<int, int>;  // distance, pageIndex
    QVector<KV> byDist;
    byDist.reserve(m_cache.size());
    for (auto it = m_cache.cbegin(); it != m_cache.cend(); ++it)
        byDist.append({qAbs(it.key() - m_currentPage), it.key()});
    std::sort(byDist.begin(), byDist.end(),
              [](const KV& a, const KV& b) { return a.first > b.first; });
    // Mo tai lieu duoc giu TOI THIEU 3 trang (trang dang xem + 2 lan can) de tai lieu
    // khong hien hanh khong bi vet sach — duoi ap luc bo nho cung con noi trang.
    const int kMinKeep = 3;
    for (const auto& kv : byDist) {
        // TUYET DOI khong don trang DANG XEM: don no di thi no render lai roi lai bi don,
        // vong lap vo tan va man hinh khong bao gio net.
        if (kv.second == m_currentPage) continue;
        if (s_globalCacheBytes.load() <= budget || m_cache.size() <= kMinKeep) break;
        qint64 sz = static_cast<qint64>(m_cache[kv.second].sizeInBytes());
        m_cacheBytes -= sz;
        s_globalCacheBytes.fetch_sub(sz);
        m_cache.remove(kv.second);
    }
}

TileKey PdfRenderer::regionKey(int page, double scale, const QRect& r) {
    return TileKey{ page, scale, r.x() / kRegionGrid, r.y() / kRegionGrid };
}

void PdfRenderer::evictRegionCache() {
    while (m_regionCacheBytes > kMaxRegionCacheBytes && !m_regionOrder.isEmpty()) {
        TileKey oldest = m_regionOrder.takeFirst();
        auto it = m_regionCache.find(oldest);
        if (it != m_regionCache.end()) {
            m_regionCacheBytes -= it->img.sizeInBytes();
            m_regionCache.erase(it);
        }
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

    bool isThumb      = (scale <= 0.25);
    bool fullQuality  = !isThumb;
    const bool suppressFull = fullQuality && m_suppressedFullQuality.contains(pageIndex);

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
                cacheInsert(pageIndex, cached);
                qDebug() << "[perf] cache add page=" << pageIndex
                         << "global=" << (s_globalCacheBytes.load() / 1048576) << "MB"
                         << "pages(tab)=" << m_cache.size();
                evictCache();
                emit pageReady(pageIndex, cached);
                return;
            }
        }
    }

    // ponytail: newest-wins for full-quality renders — cancel pending, keep at most 1
    std::shared_ptr<QAtomicInt> genRef = m_generation;
    int gen = m_generation->loadRelaxed();

    if (!suppressFull) {
        m_pendingRequests.insert(pendKey, QDateTime::currentMSecsSinceEpoch());

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
    }

    // Show any cached version (from preload) immediately (only for full-quality)
    if (fullQuality) {
        QImage placeholder = bestCachedForPage(pageIndex);
        if (!placeholder.isNull()) {
            emit pageReady(pageIndex, placeholder);
        }
    }

    // R2 (SPEC_PERF_HEAVYPAGE): trang da co lop vector ready+complete thi KHONG can
    // render raster full-quality nua (no se khong bao gio duoc ve — drawPageBase bo khi
    // pureVector). Van emit pageReady (cache hit / placeholder) o tren; chi chan viec
    // tao task render thuc su, de SAFETY NET (line ~795) cung khong phat lai.
    if (suppressFull) {
        qDebug() << "[perf] drop page=" << pageIndex << "reason=vectorReady";
        return;
    }

    bool renderAnnots = m_pageAnnotRender.value(pageIndex, true);
    bool hideOwn = renderAnnots && m_pageAnnotOverlay.value(pageIndex, false);
    RenderRequest req{pageIndex, scale, renderAnnots, fullQuality, gen, hideOwn};

    if (fullQuality) {
        auto* ptask = new ProgressiveRenderTask(this, m_doc, req, this,
                                                 genRef);
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
                    s_globalCacheBytes.fetch_add(static_cast<qint64>(img.sizeInBytes()));
                    m_cache.insert(pageIndex, img);
                    qDebug() << "[perf] cache add page=" << pageIndex
                             << "global=" << (s_globalCacheBytes.load() / 1048576) << "MB"
                             << "pages(tab)=" << m_cache.size();
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
        connect(ptask, &ProgressiveRenderTask::pageObjectCount, this, &PdfRenderer::objectCountReady);
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

    // VIỆC 2 (SPEC_VIEWFAST 2026-08-31): trang nặng không được giữ khoá pdfium cả cục
    // (lockhold 58,7s). Chuyển hẳn sang đường progressive — ProgressiveRenderTask nhả khoá
    // giữa các lát (sliceMs) và vẫn phát finished đầy đủ (không mất tín hiệu render xong).
    const bool heavyPage = isHeavyPage(pageIndex);
    auto onFinished = [this, pageIndex, fullQuality, gen](int idx, QImage img) {
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
                s_globalCacheBytes.fetch_add(static_cast<qint64>(img.sizeInBytes()));
                m_cache.insert(pageIndex, img);
                qDebug() << "[perf] cache add page=" << pageIndex
                         << "global=" << (s_globalCacheBytes.load() / 1048576) << "MB"
                         << "pages(tab)=" << m_cache.size();
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
    };

    if (heavyPage) {
        auto* ptask = new ProgressiveRenderTask(this, m_doc, req, this,
                                                 genRef);
        connect(ptask, &ProgressiveRenderTask::finished, this, onFinished);
        connect(ptask, &ProgressiveRenderTask::pageObjectCount, this, &PdfRenderer::setPageObjectCount);
        connect(ptask, &ProgressiveRenderTask::pageObjectCount, this, &PdfRenderer::objectCountReady);
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
                                    genRef);

    connect(task, &PageRenderTask::finished, this, onFinished);
    connect(task, &PageRenderTask::pageObjectCount, this, &PdfRenderer::setPageObjectCount);
    connect(task, &PageRenderTask::pageObjectCount, this, &PdfRenderer::objectCountReady);

    if (isThumb)
        m_thumbPool.start(task, 0);
    else
        m_mainPool.start(task, 10);
}

void PdfRenderer::requestRegion(int pageIndex, double scale, QRect regionPx) {
    // 🔴 LOG 2026-09-01: truoc day ham nay chi log khi TRUNG dem hoac khi XONG, nen khi
    // Continuous mo cam ta khong phan biet duoc "chua ai goi" voi "goi roi ma chua ve xong".
    qDebug().noquote() << "[region] YEU CAU page=" << pageIndex << "scale=" << scale
                       << "vung=" << regionPx << "docMo=" << (m_doc && m_doc->isOpen());
    if (!m_doc || !m_doc->isOpen()) return;

    // 2.2 Cache HIT — must be BEFORE m_regionInFlightPage and m_regionGeneration
    {
        const TileKey k = regionKey(pageIndex, scale, regionPx);
        auto it = m_regionCache.constFind(k);
        if (it != m_regionCache.constEnd() && it->rect == regionPx) {
            qDebug() << "[perf] region CACHE HIT page=" << pageIndex << "scale=" << scale;
            emit regionReady(pageIndex, scale, regionPx, it->img);
            return;
        }
    }

    if (m_regionInFlightPage == pageIndex && qAbs(m_regionInFlightScale - scale) < 1e-6 && m_regionInFlightRect == regionPx) {
        qDebug() << "[perf] region BO QUA trung yeu cau dang chay page=" << pageIndex;
        return;
    }
    m_regionInFlightPage = pageIndex;
    m_regionInFlightScale = scale;
    m_regionInFlightRect = regionPx;

    m_regionGeneration->fetchAndAddOrdered(1);
    auto genRef = m_regionGeneration;
    bool regionRenderAnnots = m_pageAnnotRender.value(pageIndex, true);
    bool regionHideOwn = regionRenderAnnots && m_pageAnnotOverlay.value(pageIndex, false);
    auto* task = new RegionRenderTask(this, m_doc,
                                       pageIndex, scale, regionPx,
                                       this, genRef, regionRenderAnnots, regionHideOwn);
    connect(task, &RegionRenderTask::finished, this,
            [this](int idx, double sc, QRect reg, QImage img) {
        m_regionInFlightPage = -1;
        if (img.isNull())
            qDebug().noquote() << "[region] TRA VE RONG page=" << idx << "scale=" << sc;
        if (!img.isNull()) {
            const TileKey k = regionKey(idx, sc, reg);
            m_regionCache.insert(k, RegionEntry{reg, img});
            m_regionOrder.append(k);
            m_regionCacheBytes += img.sizeInBytes();
            evictRegionCache();
            qDebug() << "[perf] region CACHE ghi page=" << idx
                     << "tong=" << (m_regionCacheBytes / 1048576) << "MB";
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
            cacheInsert(pageIndex, cached);
            qDebug() << "[perf] cache add page=" << pageIndex
                     << "global=" << (s_globalCacheBytes.load() / 1048576) << "MB"
                     << "pages(tab)=" << m_cache.size();
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
        // 🔴 CHOT 2026-08-31: `!isNull()` KHONG du. Anh 1x1 (hoac anh do View Fast render
        // o do phan giai thap) khong null nen tung lot qua day va duoc ve nhu anh day du
        // => TRANG TRANG/DEN. O CacheZoom::Full phai chua anh canh dai ~kFullRenderMaxPx;
        // khong dat thi coi nhu MISS va VUT entry hong di, de render lai cho dung.
        const int cachedLong = cached.isNull() ? 0 : qMax(cached.width(), cached.height());
        const int expectLong2 = int(kFullRenderMaxPx);   // o Full = luon day du
        if (!cached.isNull() && cachedLong < int(expectLong2 * 0.9)) {
            qWarning() << "[cache] VUT entry hong page=" << pageIndex
                       << "canhDai=" << cachedLong << "can>=" << int(expectLong2 * 0.9);
            cached = QImage();   // coi nhu MISS — luot render day du ke tiep se ghi de
        }
        if (!cached.isNull()) {
            qDebug() << "[perf] cache-only-for-cont hit disk page=" << pageIndex
                     << "renderedScale=" << renderedScale;
            cacheInsert(pageIndex, cached);
            qDebug() << "[perf] cache add page=" << pageIndex
                     << "global=" << (s_globalCacheBytes.load() / 1048576) << "MB"
                     << "pages(tab)=" << m_cache.size();
            evictCache();
            emit continuousPageReady(pageIndex, cached, renderedScale);
            return true;
        }
    }
    return false;
}

void PdfRenderer::requestPageForContinuous(int pageIndex, double scale, int priority)
{
    if (!m_doc || !m_doc->isOpen()) {
        qDebug() << "[perf] cont SKIP page=" << pageIndex << "reason=noDoc";
        return;
    }

    // GOC1: when m_continuousUseZoomScale is set (fast mode), render at the
    // requested zoom resolution instead of always kFullRenderMaxPx.  This
    // avoids rendering 4000px when the screen only needs ~1300px at Fit zoom.
    QSizeF pgSz = m_doc->pageSize(pageIndex);
    double longSide = qMax(pgSz.width(), pgSz.height());
    double renderedScale;
    if (m_continuousUseZoomScale) {
        // 🔴 SUA 2026-08-31: dau ngoac dat sai lam TRANG TRANG/DEN o View Fast.
        // `scale` la HE SO ZOOM (vd 0.1), khong phai so pixel. Ban cu chia CA qMin cho
        // longSide => 0.115/2381 = 4.83e-05 => trang render ra 0,115 PIXEL (do dung log:
        // "cont render start renderScale= 4.82982e-05"). Phep chia /longSide chi thuoc ve
        // CAI TRAN (doi kFullRenderMaxPx tu pixel sang he so), KHONG thuoc ve zoom.
        renderedScale = qMin(scale * 1.15,
                             PdfRenderer::kFullRenderMaxPx / qMax(longSide, 1.0));
    } else {
        renderedScale = kFullRenderMaxPx / qMax(longSide, 1.0);
    }

    // Accept any full-quality cached image (all at kFullRenderMaxPx resolution)
    if (m_cache.contains(pageIndex)) {
        qDebug() << "[perf] cont cache hit mem page=" << pageIndex
                 << "renderedScale=" << renderedScale;
        emit continuousPageReady(pageIndex, m_cache[pageIndex], renderedScale);
        return;
    }

    // VIỆC A: Dedup — nếu trang này đang render rồi, không dispatch lần thứ hai.
    // Chỉ kiểm tra, không thêm vào queue — render lần đầu sẽ phát kết quả cho tất cả.
    if (m_contPageGen.contains(pageIndex)) {
        qDebug() << "[dedup] gop yeu cau trung page=" << pageIndex;
        return;
    }

    if (m_continuousRunning.load() >= kMaxContinuousRenders) {
        // VIỆC 2: hang doi UU TIEN — giu do tang dan (0=trang chinh+lan can lam truoc).
        // Khong xep trung trang da o trong hang doi.
        bool already = false;
        for (const auto& q : m_continuousQueue)
            if (q.page == pageIndex) { already = true; break; }
        if (!already) {
            int insertAt = 0;
            while (insertAt < m_continuousQueue.size()
                   && m_continuousQueue[insertAt].priority <= priority)
                ++insertAt;
            m_continuousQueue.insert(insertAt, ContJob{pageIndex, scale, priority});
        }
        qDebug() << "[perf] cont QUEUE page=" << pageIndex
                 << "prio=" << priority << "depth=" << m_continuousQueue.size();
        return;
    }
    m_continuousRunning.fetch_add(1);

    // VIỆC 2: per-page generation — cancelContinuousPage(page) bump rieng trang nay.
    std::shared_ptr<QAtomicInt> pageGen = std::make_shared<QAtomicInt>(0);
    m_contPageGen[pageIndex] = pageGen;
    const int gen = pageGen->loadRelaxed();
    bool renderAnnots = m_pageAnnotRender.value(pageIndex, true);
    bool hideOwn = renderAnnots && m_pageAnnotOverlay.value(pageIndex, false);
    RenderRequest req{pageIndex, renderedScale, renderAnnots, true, gen, hideOwn, m_continuousUseZoomScale};

    auto* ptask = new ProgressiveRenderTask(this, m_doc, req, this,
                                             pageGen);

    // 🔴🔴 THIEU DAY 2026-09-01 — GOC CUA "Continuous khong hien comment cua phan mem khac".
    // MOI nhanh render khac (1234 · 1301 · 1318 · 1701 · 1722) deu noi `pageObjectCount`,
    // RIENG nhanh Continuous nay thi khong. Hau qua day chuyen:
    //   pageObjectCount(page) luon = 0 ⇒ ensureForeignAnnotLayer bo qua voi
    //   "[fgnlayer] DEFER - chua biet so object" ⇒ LOP BU KHONG BAO GIO DUNG ⇒ chu thich ngoai
    //   khong co gi ve no tren nen vector ⇒ Continuous mat comment, Single thi khong.
    // Day la mot SOI DAY BI QUEN, khong phai loi thuat toan — nen no am tham suot thoi gian dai.
    connect(ptask, &ProgressiveRenderTask::pageObjectCount, this, &PdfRenderer::setPageObjectCount);
    connect(ptask, &ProgressiveRenderTask::pageObjectCount, this, &PdfRenderer::objectCountReady);

    connect(ptask, &ProgressiveRenderTask::finished, this,
            [this, pageIndex, renderedScale, gen, pageGen, scale](int idx, QImage img) {
        m_continuousRunning.fetch_sub(1);
        // VIỆC 2: task da xong — trang khong con chiem khe nua. Bo gen entry de
        // isContinuousPageActive(page) ve false (khong huy nham trang da xong).
        if (m_contPageGen.contains(pageIndex) && m_contPageGen[pageIndex] == pageGen)
            m_contPageGen.remove(pageIndex);
        if (!m_continuousQueue.isEmpty()) {
            // VIỆC 2: rut ra job UU TIEN NHAT (priority nho nhat). Cung muc uu tien
            // thi lay LIFO (moi nhat truoc).
            int bestIdx = 0;
            for (int i = 1; i < m_continuousQueue.size(); ++i)
                if (m_continuousQueue[i].priority < m_continuousQueue[bestIdx].priority)
                    bestIdx = i;
            const ContJob nx = m_continuousQueue.takeAt(bestIdx);
            qDebug() << "[perf] cont DEQUEUE page=" << nx.page
                     << "prio=" << nx.priority << "con lai=" << m_continuousQueue.size();
            QMetaObject::invokeMethod(this, [this, nx]{ requestPageForContinuous(nx.page, nx.scale, nx.priority); }, Qt::QueuedConnection);
        }
        if (img.isNull()) {
            // 🔴 SUA 2026-08-31 — GOC CUA "trang quai vat chi hien mot it net, mat khung ten".
            // Anh RONG o day = luot render bi CAT GIUA CHUNG. Cac lat ve dan (pagePartial) DA
            // nam tren man hinh, nhung nhanh phat anh day du khong chay => lat ve DO cuoi cung
            // O NGUYEN do vinh vien. Khung ten nam o goc, la phan ve SAU CUNG, nen mat dau tien.
            // Cung co che nay giai thich "trang 2, 3 mo lau hon" — chung bi cat ngang nhieu lan
            // hon trang chinh, chu khong phai vi nang hon.
            // ⇒ DAT LAI lenh render thay vi bo. Co tran kContNullRetryMax de khong lap vo tan.
            // 🔴 BO SUNG 2026-08-31: phan biet BI HUY voi THAT BAI.
            // Sau khi chan anh ve do (xem ProgressiveRenderTask), moi lan HUY cung tra ve anh
            // rong. Neu cu the ma dat lai lenh thi lenh dat lai se DANH NHAU voi chinh lenh huy:
            // khung nhin vua bao "trang nay khong con nhin thay, bo di" thi day lai xin render.
            // The he da doi = bi huy co chu y => im lang, de khung nhin tu xin lai khi can.
            if (gen != pageGen->loadRelaxed()) {
                qDebug() << "[perf] cont page=" << idx << "bi HUY (khong phai loi) — khong dat lai";
                return;
            }
            const int nRetry = m_contNullRetry.value(idx, 0);
            if (nRetry < kContNullRetryMax) {
                m_contNullRetry[idx] = nRetry + 1;
                qDebug() << "[perf] cont anh RONG page=" << idx
                         << "— dat lai lan" << (nRetry + 1) << "/" << kContNullRetryMax;
                QMetaObject::invokeMethod(this, [this, idx, scale]{
                    requestPageForContinuous(idx, scale);
                }, Qt::QueuedConnection);
            } else {
                qDebug() << "[perf] cont drop page=" << idx << "reason=nullResult (het luot thu lai)";
            }
            return;
        }
        m_contNullRetry.remove(idx);   // thanh cong => xoa bo dem thu lai
        if (gen != pageGen->loadRelaxed()) {
            // 🔴 SUA 2026-08-31 — GOC CUA "trang quai vat cho mai khong hien".
            // Do duoc tren trang 2,54 trieu doi tuong: render XONG sau 6.173 ms nhung ket qua
            // bi vut ca 2 luot (genMismatchMid + genStale), KHONG luot nao toi man hinh.
            // Trang mat 6 giay thi trong 6 giay do chi can MOT su kien tang the he (cuon nhe,
            // dung thumbnail, doi tab) la mat trang cong — roi lam lai, roi lai mat.
            // NHUNG PIXEL VAN DUNG: noi dung trang khong doi, chi thu tu uu tien doi.
            // => KHONG vut nua. CAT VAO BO DEM de lan sau hoi toi la co ngay.
            // Van khong phat continuousPageReady (vi yeu cau da cu), chi giu lai cong suc.
            if (!img.isNull() && !m_cache.contains(pageIndex)) {
                m_cacheBytes += static_cast<qint64>(img.sizeInBytes());
                s_globalCacheBytes.fetch_add(static_cast<qint64>(img.sizeInBytes()));
                m_cache.insert(pageIndex, img);
                qDebug() << "[perf] cont genStale — GIU vao bo dem page=" << idx
                         << "imgW=" << img.width();
                evictCache();
            } else {
                qDebug() << "[perf] cont drop page=" << idx << "reason=genStale";
            }
            return;
        }
        if (!m_cache.contains(pageIndex)) {
            m_cacheBytes += static_cast<qint64>(img.sizeInBytes());
            s_globalCacheBytes.fetch_add(static_cast<qint64>(img.sizeInBytes()));
            m_cache.insert(pageIndex, img);
            qDebug() << "[perf] cache add page=" << pageIndex
                     << "global=" << (s_globalCacheBytes.load() / 1048576) << "MB"
                     << "pages(tab)=" << m_cache.size();
        }
        evictCache();

        // 🔴 CHOT 2026-08-31: View Fast render o scale zoom (vd 274px) — KHONG duoc ghi
        // vao o CacheZoom::Full, vi lan doc sau se tuong do la anh day du 4000px.
        // Day chinh la co che DAU DOC cache: mot lan render hong la hong VINH VIEN
        // tren dia, sua code cung khong cuu duoc cho toi khi xoa file cache.
        // 🔴 SUA 2026-08-31 (lan 2): truoc day so voi kFullRenderMaxPx*0.9 = 3600px. Sau khi
        // chuyen sang siet theo DPI thi o 75 DPI trang A0 chi 2480px => chot NAY CHAN SACH
        // viec ghi dem, moi lan mo lai phai render lai tu dau. Phai so voi KY VONG theo DPI.
        const int outLong = qMax(img.width(), img.height());
        // O Full chi nhan anh DAY DU — day la giao uoc chung voi Single.
        const bool fullQualityImg = outLong >= int(PdfRenderer::kFullRenderMaxPx * 0.9);
        if (!fullQualityImg)
            qDebug() << "[cache] KHONG ghi dia page=" << pageIndex
                     << "canhDai=" << outLong << "(chua du chat luong day du)";
        if (fullQualityImg && m_tileCache && m_tileCache->isOpen() && !m_writingCache.contains(pageIndex)) {
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

    qDebug() << "[perf] cont render start page=" << pageIndex
             << "renderScale=" << renderedScale << "zoom=" << scale
             << (m_continuousUseZoomScale ? "(fast)" : "(quality)");
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
    // Dang render trang chinh o chat luong day du -> KHONG preload, se cuop khoa cua no.
    if (m_fullRenderRunning) {
        qDebug() << "[perf] preload SKIP (full render dang chay) page=" << pageIndex;
        return;
    }
    int total = m_doc->pageCount();
    // Preload only 1 page each side (each full 4000px render costs ~4.4s on heavy pages)
    for (int delta : {1, -1}) {
        int idx = pageIndex + delta;
        if (idx < 0 || idx >= total) continue;
        if (m_cache.contains(idx)) continue;
        if (pageObjectCount(idx) > 400000) continue;
        qint64 pendKey = pendingCacheKey(idx, true);
        if (m_pendingRequests.contains(pendKey)) {
            qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - m_pendingRequests.value(pendKey) < 30000) continue;
            m_pendingRequests.remove(pendKey);
        }
        m_pendingRequests.insert(pendKey, QDateTime::currentMSecsSinceEpoch());
        int gen = m_generation->loadRelaxed();
        RenderRequest req{idx, 1.0, false, true, gen};
        // VIỆC 2 (SPEC_VIEWFAST 2026-08-31): trang nặng (hoặc chưa đếm → giả định nặng)
        // không được giữ khoá pdfium cả cục — dùng ProgressiveRenderTask nhả khoá từng lát.
        if (isHeavyPage(idx)) {
            auto* ptask = new ProgressiveRenderTask(this, m_doc, req, this,
                                                     m_generation);
            connect(ptask, &ProgressiveRenderTask::finished, this,
                    [this, idx](int /*idx2*/, QImage img) {
                qint64 fk = pendingCacheKey(idx, true);
                m_pendingRequests.remove(fk);
                if (!img.isNull() && !m_cache.contains(idx)) {
                    m_cacheBytes += static_cast<qint64>(img.sizeInBytes());
                    s_globalCacheBytes.fetch_add(static_cast<qint64>(img.sizeInBytes()));
                    m_cache.insert(idx, img);
                    qDebug() << "[perf] cache add page=" << idx
                             << "global=" << (s_globalCacheBytes.load() / 1048576) << "MB"
                             << "pages(tab)=" << m_cache.size();
                    evictCache();
                }
            });
            connect(ptask, &ProgressiveRenderTask::pageObjectCount, this, &PdfRenderer::setPageObjectCount);
            connect(ptask, &ProgressiveRenderTask::pageObjectCount, this, &PdfRenderer::objectCountReady);
            m_thumbPool.start(ptask, 0);
            continue;
        }
        auto* task = new PageRenderTask(this, m_doc, req, this,
                                        m_generation);
        connect(task, &PageRenderTask::finished, this,
                [this, idx](int /*idx2*/, QImage img) {
            qint64 fk = pendingCacheKey(idx, true);
            m_pendingRequests.remove(fk);
            if (!img.isNull() && !m_cache.contains(idx)) {
                m_cacheBytes += static_cast<qint64>(img.sizeInBytes());
                s_globalCacheBytes.fetch_add(static_cast<qint64>(img.sizeInBytes()));
                m_cache.insert(idx, img);
                qDebug() << "[perf] cache add page=" << idx
                         << "global=" << (s_globalCacheBytes.load() / 1048576) << "MB"
                         << "pages(tab)=" << m_cache.size();
                evictCache();
            }
        });
        connect(task, &PageRenderTask::pageObjectCount, this, &PdfRenderer::setPageObjectCount);
        connect(task, &PageRenderTask::pageObjectCount, this, &PdfRenderer::objectCountReady);
        m_thumbPool.start(task, 0);
    }
}

void PdfRenderer::clearCache() {
    m_generation->fetchAndAddOrdered(1);
    m_fullRenderGen->fetchAndAddOrdered(1);
    m_progressiveGen->fetchAndAddOrdered(1);
    m_continuousGen->fetchAndAddOrdered(1);
    m_tileGeneration->fetchAndAddOrdered(1);
    for (auto& kv : m_contPageGen) kv->fetchAndAddOrdered(1);
    m_contPageGen.clear();
    m_continuousQueue.clear();
    s_globalCacheBytes.fetch_sub(m_cacheBytes);
    m_cacheBytes = 0;
    m_cache.clear();
    m_tileCacheInMem.clear();
    m_tileCacheBytes = 0;
    m_pendingTiles.clear();
    m_regionCache.clear();
    m_regionOrder.clear();
    m_regionCacheBytes = 0;
    m_pageObjectCount.clear();
    m_pageAnnotOverlay.clear();
    m_suppressedFullQuality.clear();
    m_fullRenderRunning = false;
}

void PdfRenderer::invalidatePage(int pageIndex) {
    auto it = m_cache.find(pageIndex);
    if (it != m_cache.end()) {
        qint64 sz = static_cast<qint64>(it.value().sizeInBytes());
        m_cacheBytes -= sz;
        s_globalCacheBytes.fetch_sub(sz);
        m_cache.erase(it);
    }
    for (auto rit = m_regionCache.begin(); rit != m_regionCache.end(); ) {
        if (rit.key().page == pageIndex) {
            m_regionCacheBytes -= rit->img.sizeInBytes();
            m_regionOrder.removeAll(rit.key());
            rit = m_regionCache.erase(rit);
        } else ++rit;
    }
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
