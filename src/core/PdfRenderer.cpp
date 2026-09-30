#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "PdfRenderer.h"
#include "PdfCoords.h"
#include "PageCache.h"
#include "PdfiumLock.h"
#include "DocTaskGate.h"
#include "Bisect.h"
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

// 0927 LƯỢT 8: trần chờ pool render. Đủ cho task cắt ở slice kế tiếp (slice =
// 50 ms, xem ProgressivePauseCtx::sliceMsFromEnv) + thời gian trả pool handle.
// Trang CAD nặng nhất đo được (quaivat.pdf) render 14,7 s — cần CẮT, không cần chờ.
static const int kPoolWaitMs = 3000;

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

// ── [closeorder] 0927 LƯỢT 2: FORM FILL ───────────────────────────────────────
// PDFium (fpdf_formfill.h) noi ro:
//   "The FPDF_FORMFILLINFO passed in via |formInfo| must remain valid until the
//    returned FPDF_FORMHANDLE is closed."
// ⇒ `ffi` BAT BUOC phai cung so voi `form`. Truoc day TileBatchRenderTask khai
// bao `ffi` TRONG khoi `if (hasForms)` nen no CHET truoc FPDF_FFLDraw va
// truoc FPDFDOC_ExitFormFillEnvironment ⇒ moi dung doc la doc rác, va
// CPDFSDK_FormFillEnvironment giu con tro treo (ke ca FPDF_PAGE da ghi qua
// FORM_OnAfterLoadPage) ⇒ duong nguon CHECK 0x80000003 khi dong trang/doc.
// ⇒ O day: mot RAII duy nhat — `ffi` o HAM, va LUON co FORM_OnBeforeClosePage
// truoc moi FPDF_ClosePage cua trang da tung FORM_OnAfterLoadPage, roi
// FPDFDOC_ExitFormFillEnvironment. Het scope = da dong sach.
struct FormEnv {
    FPDF_FORMFILLINFO ffi{};      // PHAI o HAM, khong phai bien cuc bo cua khoi con
    FPDF_FORMHANDLE   form = nullptr;
    FPDF_PAGE         page = nullptr;
    FPDF_DOCUMENT     doc  = nullptr;

    void open(FPDF_DOCUMENT d, FPDF_PAGE pg, const char* where) {
        doc = d; page = pg;
        if (!d || !pg) return;
        if (FPDF_GetFormType(d) == FORMTYPE_NONE) return;   // an toan: doc khong co form
        std::memset(&ffi, 0, sizeof(ffi));
        ffi.version = 2;
        form = FPDFDOC_InitFormFillEnvironment(d, &ffi);
        if (!form) return;
        Q_UNUSED(where);
        FORM_OnAfterLoadPage(pg, form);      // PDFium: bat buoc truoc FPDF_FFLDraw
    }
    void draw(FPDF_BITMAP bmp, int w, int h, int flags) {
        if (form) FPDF_FFLDraw(form, bmp, page, 0, 0, w, h, 0, flags);
    }
    void close() {
        if (!form) return;
        FORM_OnBeforeClosePage(page, form);
        FPDFDOC_ExitFormFillEnvironment(form);
        form = nullptr; page = nullptr; doc = nullptr;
    }
    ~FormEnv() { close(); }
    // 🔴 Phai khai bao TAY: mot copy-ctor (ke ca `= delete`) da "user-declared"
    // nen implicit default-ctor bi KHONG sinh ⇒ `FormEnv f;` loi C2512.
    FormEnv() = default;
    FormEnv(const FormEnv&) = delete;
    FormEnv& operator=(const FormEnv&) = delete;
};

bool PdfRenderer::waitIdle(int ms) {
    // 🔴 0927 LƯỢT 8. KHÔNG dùng waitForDone() vô hạn: trang CAD nặng render 14 s, chờ
    // vô hạn treo cửa sổ lúc Alt+F4. Vòng lặp có hạn + nhả theo slice để log vẫn đúng
    // thời gian thực.
    bool ok = false;
    for (int waited = 0; waited <= ms && !ok; waited += 20) {
        const int slice = qMin(20, ms - waited);
        ok = m_thumbPool.waitForDone(slice) && m_mainPool.waitForDone(slice);
    }
    if (ok) { PdfCloseTrace::note("WAIT-POOL-OK", QStringLiteral("main+thumb sach")); return true; }
    PdfCloseTrace::note("WAIT-POOL-HO",
        QStringLiteral("main=%1 thumb=%2 con chay sau %3 ms — van dung tai lieu, KHONG duyet de pha")
            .arg(m_mainPool.activeThreadCount()).arg(m_thumbPool.activeThreadCount()).arg(ms));
    return false;
}

// 🔴 0928 LƯỢT 23 — TRẦN NHƯỜNG CỦA THUMBNAIL PHAI BẰNG THỜI GIAN THỰC CỦA TRANG.
// Đo r23 (lap 2, thumbnail bật): render trang 3 Phần ngầm chờ khoá 19,1 s chỉ để
// làm 8,1 s việc; 455 lát, mỗi lát chỉ vượt ~1 ranh giới object. Lý do: trần nhường
// 5 s (L19) tính từ lúc primary BẮT ĐẦU chờ, mà ContinuousView đặt nó lúc đó chưa
// biết trang này nặng bao nhiêu — pre-count của PDFium chưa chạy. Nơi duy nhất biết
// sớm nhất là đúng chỗ pre-count, nên vũ khí nặng được giao ở đây: trang ≥1 M object
// mà ĐANG là trang primary chờ ảnh ⇒ kéo trần lên 30 s (đúng trần phe do dung cho
// mot trang). Hết 30 s thumbnail vẫn vẽ — chống đói giữ nguyên.
static void armHeavyThumbYield(int page, int nObj) {
    if (nObj < 1000000) return;
    if (g_pdfiumUrgentPage().load(std::memory_order_acquire) != page) return;
    g_pdfiumUrgentPageDeadline().store(QDateTime::currentMSecsSinceEpoch() + 30000,
                                       std::memory_order_release);
}

// 0928 LƯỢT 24 — [lat] in ra MỖI lát (~500 dong/trang nang) theo reviewer L23:
// boc sau env TORREADER_LATLOG=1, mac dinh TAT. Vong dem can thi bat.
static bool latLogOn() {
    static const bool on = qEnvironmentVariableIsSet("TORREADER_LATLOG");
    return on;
}

// 🔴 0928 LƯỢT 26 (VIỆC 1) — CPU THỰC của luồng render cho MỘT lượt vẽ trang.
// Phân biệt dứt điểm "làm nhiều việc hơn" (cpu≈wall) vs "bị cướp CPU/ngủ"
// (cpu<<wall) mà không cần đào tiếp PDFium. GetThreadTimes chỉ đếm thời gian
// luồng NÀY chạy trên nhân ⇒ lúc chờ khoá / chờ tới lượt chạy không tính vào cpu.
// Windows-only (bọc #ifdef), LUÔN BẬT — 1 dòng/lượt, in ở cuối lượt + khi huỷ.
struct ThreadCpuAcc {
    qint64 cpuMs = 0;
#ifdef Q_OS_WIN
    qint64 lastU = -1, lastK = -1;
    static qint64 ft(const FILETIME& f) {
        LARGE_INTEGER li; li.LowPart = f.dwLowDateTime; li.HighPart = f.dwHighDateTime;
        return li.QuadPart / 10000;   // don vi 100ns -> ms
    }
    void snap() {   // goi sau moi lat: cong don user+kernel tu lan snap truoc
        FILETIME c, e, u, k;
        if (GetThreadTimes(GetCurrentThread(), &c, &e, &u, &k)) {
            const qint64 uu = ft(u), kk = ft(k);
            if (lastU >= 0) cpuMs += (uu - lastU) + (kk - lastK);
            lastU = uu; lastK = kk;
        }
    }
    static int prio() { return GetThreadPriority(GetCurrentThread()); }
#else
    void snap() {}
    static int prio() { return -99; }
#endif
};

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
            // 🔴 LƯỢT 30 (VIỆC 2, theo SỐ): --ram-probe chứng minh kho ảnh raster
            // (globalCache) là GIỎ LỚN NHẤT — dính trần ở MỌI cỡ pool, ~46% peak.
            // Chủ nhân đòi commit ≤ 3 GB cho 2 file. Trần 2 GB (r28) ⇒ peak 4,7 GB.
            // Trần 1 GB ⇒ peak 3,2 GB (VẪN vượt 3000). Trần 512 MB ⇒ peak 2,95 GB (ĐẠT).
            // Mở lại trang hiện từ .torcache trên đĩa (fix C) nên kho RAM nhỏ không vẽ lại.
            b = qBound(384LL * MB, b, 512LL * MB);         // kep [384 MB, 512 MB]
        } else {
            b = 512LL * MB;                                 // khong lay duoc RAM: 512 MB
        }
        // 🔴 LƯỢT 30 (VIỆC 2 — theo SỐ): ram-probe chứng minh globalCache (ảnh raster)
        // là GIỎ chứa LỚN NHẤT (2 GB trần, pinned ở MỌI cỡ pool) — nghi phạm A (pool doc)
        // SAI (pool 12 vs 1 chỉ khác 8%). Trần RAM chủ nhân đòi (≤3 GB cho 2 file) ⇒
        // hạ trần kho ảnh. TORREADER_CACHE_MB=<MB> cho phép đo/đổi mà không rebuild.
        bool ok = false;
        const int envMB = qEnvironmentVariableIntValue("TORREADER_CACHE_MB", &ok);
        if (ok && envMB > 0) b = static_cast<qint64>(envMB) * MB;
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
    // 🔴 0928 LƯỢT 22: token ĐĂNG KÝ LÚC SPAWN (ctor chạy trên luồng gọi, trước
    // pool->start) — task xếp hàng trong pool cũng hiện trong sổ beginClose.
    , m_task(m_pdfDoc ? m_pdfDoc->raw() : nullptr, "PageRenderTask")
{
    setAutoDelete(true);
    connect(this, &PageRenderTask::finished, receiver,
            [](int, QImage){}, Qt::QueuedConnection);
}


void PageRenderTask::run() {
    // 0928 LƯỢT 14: acquirePage() mượn FPDF_PAGE của doc CHÍNH qua PageCache.
    // waitIdle() có trần (3 s) nên quá trần thì doc vẫn bị đóng ⇒ token này giữ
    // doc sống tới khi task thật sự xong, kể cả lúc đó. LƯỢT 22: token dời vào
    // CTOR (m_task) — đăng ký lúc spawn, huỷ khi QRunnable bị xoá sau run.
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

        // V1b 0901: biet so object TRUOC khi chon tran. Lenh emit pageObjectCount o cuoi
        // chi chay SAU khi ve xong => trang ve 4000px khong bao gio xong => tran mai la 4000
        // => long khoa chan-ga-quay. Do ngay luc vua co page (chi doc danh sach object, re).
        // 0927 LƯỢT 10 (--no-precount): BO khoi dem FPDFPage_CountObjects o day.
        // setPageObjectCount khong chay ⇒ pageObjectCount luon = 0 ⇒ fullQCapPx tra
        // kFullRenderMaxPx cho MOI trang (trang nang cung ve 4000px) — dung y do la
        // thay doi HANH VI CANH DO, dung khi tach phan.
        if (!trNoPreCount()) { QElapsedTimer ct; ct.start();
          const int nObj = FPDFPage_CountObjects(page);
          if (!m_renderer->isClosing())   // L33h: đừng ghi hash của renderer đang đóng (luồng nền)
            m_renderer->setPageObjectCount(m_req.pageIndex, nObj);
          qDebug() << "[heavycap] pre-count page=" << m_req.pageIndex << "objects=" << nObj
                   << "ms=" << ct.elapsed();
          armHeavyThumbYield(m_req.pageIndex, nObj); }   // 0928 LƯỢT 23

        double longSide = qMax(w, h);
        double maxPx = m_req.fullQuality ? m_renderer->fullQCapPx(m_req.pageIndex, longSide)
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
        // 0927 LƯỢT 2: FORM FILL qua FormEnv (ffi + form cung so mot, LUON co
        // FORM_OnBeforeClosePage truoc khi trang rời khoi ham). Ban cũ viet tay,
        // deo khoi nhac do doc vao FPDF_ClosePage/FPDF_CloseDocument.
        FormEnv _form;
        _form.open(m_pdfDoc ? m_pdfDoc->raw() : nullptr, page, "PageRenderTask::run");
        if (_form.form)
            _form.draw(bmp, imgW, imgH, FPDF_ANNOT | FPDF_RENDER_LIMITEDIMAGECACHE);
        _form.close();
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
    // 🔴 0928 LƯỢT 22: token lúc SPAWN (xem PageRenderTask).
    , m_task(m_pdfDoc ? m_pdfDoc->raw() : nullptr, "ProgressiveRenderTask")
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
        // 0927 LƯỢT 9: đường phòng thủ — m_bmp đã bị huỷ DƯỚI KHOÁ ở run[pool]/run[PageCache]
        // nên gần như không tới đây, nhưng FPDFBitmap_Destroy vẫn là lệnh PDFium.
        MaybePdfiumLock lk(__FILE__, __LINE__, trPoolLockOn(), m_req.pageIndex);
        FPDFBitmap_Destroy(m_bmp);
        m_bmp = nullptr;
    }
}

void ProgressiveRenderTask::run() {
    // 0928 LƯỢT 14: xem PageRenderTask::run() — cùng lý do, và task này còn giữ
    // FPDF_PAGE riêng (m_fpdfPage) suốt nhiều lát progressive. LƯỢT 22: m_task.
    if (!m_genRef || m_genRef->loadRelaxed() != m_req.generation) {
        qDebug() << "[perf] drop page=" << m_req.pageIndex << "reason=genMismatch" << "fullQuality=" << m_req.fullQuality << "thread=" << QThread::currentThreadId();
        emit finished(m_req.pageIndex, QImage()); return;
    }

    // 🔴 LƯỢT 33b (J — tab nặng chặn tab mới): tab NỀN (không phải tab đang xem)
    // KHÔNG được BẮT ĐẦU FPDF_LoadPage trang quái vật — nó giữ s_pdfiumMutex
    // ~1,7 s (đo r31b: PdfRenderer.cpp:464 giu ms=1724 trang=3), đúng lúc tab vừa
    // mở cần khoá để FPDF_LoadMemDocument + trang đầu. Trang nền sẽ vẽ lại khi tab
    // thành hiện hành (onTabChanged → setDocument → requestVisiblePages).
    // 🔴 LƯỢT 33d (J): activeDoc NULL không còn là "cho qua" — khoảng trống tab mới
    // đang mở (setActiveDoc(raw=null) cho tới lúc doc mở xong) chính là lúc f2 cần
    // khoá nhất. Đo r33b JKL: lượt vẽ trang 3 f3 BẮT ĐẦU sau khi f2 mở (log 284 >
    // 275) mà vẫn lọt qua chốt cũ vì activeDoc đang null ⇒ giữ khoá 1749 ms chặn
    // FPDF_LoadMemDocument của f2. Nay: null + có việc khẩn (scope mở doc) ⇒ nền
    // đứng lại. Không khẩn ⇒ cho qua (đừng chặn nhầm lúc không ai chờ).
    if (m_pdfDoc) {
        const FPDF_DOCUMENT a = PageCache::activeDoc();
        if ((a && m_pdfDoc->raw() != a) || (!a && pdfiumUrgentPending())) {
            qDebug() << "[perf] drop page=" << m_req.pageIndex << "reason=tab-nen";
            emit finished(m_req.pageIndex, QImage()); return;
        }
    }

    qDebug() << "[perf] progressive start page=" << m_req.pageIndex
             << "scale=" << m_req.scaleFactor
             << "fullQuality=" << m_req.fullQuality
             << "thread=" << QThread::currentThreadId();

    // 🔴 0928 LƯỢT 13 — ĐÂY LÀ LỐI VÀO ƯU TIÊN. Trang đang hiển thị thì bất kỳ
    // lát PDFium nào của nó cũng được quyền khoá trước; thumbnail + OCR probe
    // nhìn thấy số đếm > 0 và lùi lại. Trước đây không có gì phân biệt, nên log
    // khoachung cho thấy đường raster của TRANG ĐANG XEM chờ 2346 ms sau một
    // hàng thumbnail 350 việc.
    // Task cũ (sau khi đổi trang) tự thoát ở kiem gen o dau ham hoac giua vong
    // lap ⇒ urgent khu vuc cung dung thoi, khong chan thumbnail vo han.
    const bool laTrangHienThi = (m_renderer->urgentPage() == m_req.pageIndex);
    UrgentPdfiumScope _urgent(laTrangHienThi);
    // 🔴 LOG-ONLY 2026-09-01: hai co quyet dinh chu thich co duoc ve vao anh khong.
    qDebug().noquote() << "[annotflag] page=" << m_req.pageIndex
                       << "veChuThich=" << m_req.renderAnnotations
                       << "giauMarkupCuaTa=" << m_req.hideOwnAnnots;

    // 🔴 0927 LƯỢT 12 — GHI CHÚ SAI ĐÃ SỬA. Bản gốc ghi "mượn handle từ pool để
    // render mà KHÔNG cần s_pdfiumMutex" và suy ra "PDFium an toàn khi hai luồng dùng
    // hai FPDF_DOCUMENT khác nhau". Suy luận đó SAI, và CEO đã đo được:
    //   • ASan: heap-use-after-free trong CPDF_Color::~CPDF_Color lúc FPDF_ClosePage —
    //     đối tượng bị thả là ColorSpace TOÀN CỤC (CPDF_ColorSpace::InitializeGlobals
    //     / GetStockCS) dùng chung MỌI FPDF_DOCUMENT, RetainPtr đếm KHÔNG nguyên tử.
    //   • TSan: 229 tranh chấp dính pdfium (PageCache.cpp:524/280/705 ↔
    //     ThumbnailRenderPool.cpp:152/245 ↔ PdfRenderer.cpp:367/425).
    // Bằng chứng gốc: crash_dump_evidence_0927/asan_run*_heap_use_after_free.txt,
    // tsan_khongkhoa.txt, tsan_cokhoa.txt. Báo cáo: REPORT_CRASH_EYACHO_0927.md.
    //
    // Handle pool KHÔNG tạo vùng trạng thái riêng — nó chỉ tách cấu trúc
    // parse riêng theo handle. Nên "render song song không khoá" là đường đã biết
    // HỎNG. Giờ đây nhánh pool ĐÃ khoá đúng như mọi nhánh khác (xem lkPoolStart
    // bên dưới); vẫn NHẢ khoá giữa các lát để giao diện không đứng.
    //
    // ⚠️ Vẫn giữ quy tắc: trang có chú thích MỚI phải vẽ từ TÀI LIỆU CHÍNH (bản sao
    // trong pool không chứa chúng) — `pageAnnotDirty` bên dưới.
    // 🔴 LƯỢT 33d (J): CHOT KHAN TRUOC KHI MUON HANDLE/GIU KHOA lat Start. Do r33d:
    // task ve trang 3 f3 entry luc activeDoc van la f3 (fen true) ⇒ thoat chot nen
    // o dau ham, roi nam LOCK 1726 ms dung ngay cua so f2 mo. Nay: co khan khac
    // dang cho (scope mo doc HOAC cua so trang-dau) va day KHONG phai trang khan
    // ⇒ ngu toi da 2 s, tynh lai; neu da thanh tab nen ⇒ BO (view se xin lai khi
    // tab hien hanh). Trang chinh cua tab hien hanh (laTrangHienThi) khong bi chan.
    if (m_pdfDoc && !laTrangHienThi) {
        int guard = 0;
        while (guard++ < 100
               && (pdfiumUrgentPending()
                   || (g_pdfiumUrgentPage().load(std::memory_order_acquire) >= 0
                       && QDateTime::currentMSecsSinceEpoch()
                          <= g_pdfiumUrgentPageDeadline().load(std::memory_order_acquire)))
               && m_req.pageIndex != g_pdfiumUrgentPage().load(std::memory_order_acquire)
               && m_genRef->loadRelaxed() == m_req.generation)
            QThread::msleep(20);
        const FPDF_DOCUMENT a2 = PageCache::activeDoc();
        if (a2 && m_pdfDoc->raw() != a2) {
            qDebug() << "[perf] drop page=" << m_req.pageIndex
                     << "reason=tab-nen-truoc-khoa";
            emit finished(m_req.pageIndex, QImage()); return;
        }
    }
    // 🔴 LƯỢT 33e (mục 5 — J, nguyên tắc "null ⇒ nhường khi khẩn"): ngay cả trang
    // ĐANG HIỂN THỊ cũng NHƯỜNG khi (a) có tab khác ĐANG MỞ (UrgentPdfiumScope —
    // ngắn 0,1–0,5 s) HOẶC (b) cửa sổ "trang đầu chờ ảnh" (g_pdfiumUrgentPage,
    // openFile đặt 1,5 s / open-finished đặt 5 s) đang nhắm trang KHÁC. Đo r33e:
    // task f3-trang-3 khởi động đúng khe open xong nhưng open-finished chưa chạy —
    // khẩn scope đã tắt, cửa sổ trang-0 đang bật — chốt cũ chỉ nhìn scope ⇒ lọt,
    // parse quái vật 1749 ms chặn ngay lượt vẽ trang 0 f2. Trang hiển thị trễ
    // ~0,5 s, đổi lại tab mới có ảnh < 1,5 s (đúng phàn nàn owner). Trần 2 s.
    if (m_pdfDoc && laTrangHienThi) {
        auto canNhoCuaSo = [this] {
            const int up = g_pdfiumUrgentPage().load(std::memory_order_acquire);
            // 🔴 LƯỢT 33f (mục 2 — reviewer): cửa sổ chỉ chặn khi thuộc doc KHÁC
            // (tab khác / tab đang mở — token 0). Cùng tab ⇒ trang hiển thị không bị
            // trang phụ của chính tab chặn trọn 5 s.
            if (up >= 0 && up != m_req.pageIndex
                && g_pdfiumUrgentDoc().load(std::memory_order_acquire)
                       != (uintptr_t)m_pdfDoc->raw()
                && QDateTime::currentMSecsSinceEpoch()
                   <= g_pdfiumUrgentPageDeadline().load(std::memory_order_acquire))
                return true;
            // 🔴 LƯỢT 33f (mục 1): nhường chỉ khi có việc khẩn KHÁC — UrgentPdfiumScope
            // của CHÍNH task này đã tăng bộ đếm từ lúc vào hàm ⇒ đếm quá 1 mới là khẩn
            // khác (hồi quy 33e: tự nhường, ngủ trọn trần 2 s mỗi lượt vẽ).
            return pdfiumUrgentPendingBeyond(1);
        };
        if (canNhoCuaSo()) {
            int guard = 0;
            while (guard++ < 100 && canNhoCuaSo()
                   && m_genRef->loadRelaxed() == m_req.generation)
                QThread::msleep(20);
            // 🔴 LƯỢT 33e (mục 5 — J, đo r33e): sau giấc nhường, tab gần như chắc
            // ĐÃ thành nền (doc khẩn vừa mở xong + activeDoc đổi). Không kiểm lại ⇒
            // task phụ tỉnh dậy rồi FPDF_LoadPage trang quái vật LẦN HAI (đo:
            // LOADPAGE 1647 + 1651 ms cho CÙNG trang 3) ⇒ J phụt 2776 ⇒ 8029 ms.
            // Bỏ — view xin lại khi tab thành hiện hành.
            const FPDF_DOCUMENT a3 = PageCache::activeDoc();
            if (a3 && m_pdfDoc->raw() != a3) {
                qDebug() << "[perf] drop page=" << m_req.pageIndex
                         << "reason=tab-nen-sau-nhuong-khan";
                emit finished(m_req.pageIndex, QImage()); return;
            }
        }
    }
    if (m_renderer->pageAnnotDirty(m_req.pageIndex)) {
        m_poolHandle = nullptr;
    } else {
        m_poolHandle = m_renderer->borrowPoolHandle();
    }

    std::unique_ptr<OwnAnnotHideGuard> _ahGuard;
    QElapsedTimer renderTimer;
    renderTimer.start();
    // 0928 LƯỢT 26 (VIỆC 1): cong don CPU that cua luồng render cho lượt này.
    ThreadCpuAcc cpuAcc;
    cpuAcc.snap();

    // ── Step 1: FPDF_RenderPageBitmap_Start ───────────────────────────────────
    int startStatus = FPDF_RENDER_READY;

    // 🔴 0927 LƯỢT 12 — GHI CHÚ SAI ĐÃ SỬA. Bản gốc ghi "nếu có pool handle thì KHÔNG
    // cần s_pdfiumMutex, chạy song song". Đo bằng TSan/ASan (crash_dump_evidence_0927/)
    // đã chứng minh điều đó sai: 229 tranh chấp dính pdfium, và use-after-free trên
    // ColorSpace toàn cục lúc FPDF_ClosePage — dù là handle RIÊNG. Nay nhánh pool
    // khoá ĐÚNG NHƯ nhánh PageCache, chỉ khác ở chỗ nằm trên handle riêng.
    if (m_poolHandle) {
        qDebug() << "[song bang] trang=" << m_req.pageIndex << "dung pool handle";

        // Lát Start: giữ khoá tới hết lát, rồi `unlock()` ngay dưới — TRƯỚC vòng
        // Continue, đúng nhịp nhả khoá của nhánh PageCache để UI không đứng.
        // 🔴 LƯỢT 33e (mục 5 — J): TÁCH lát Start thành HAI vùng khoá —
        //   vùng 1: FPDF_LoadPage (đo riêng bằng [lat] LOADPAGE — đây là nghi phạm
        //   giữ 1,7 s của trang quái vật 2,18M object, giữa chứng KHÔNG cắt được);
        //   khe giữa: NHƯỜNG nếu có mở doc khẩn đang chờ (khan ngắn, vào được khe
        //   này trước khi vùng 2 khởi động);
        //   vùng 2: pre-count + FPDF_RenderPageBitmap_Start.
        // Mục đích: cửa sổ f2 mở không phải chờ trọn lát gộp của f3.
        FPDF_PAGE poolPage = nullptr;
        {
            MaybePdfiumLock lkLoad(__FILE__, __LINE__, trPoolLockOn(), m_req.pageIndex);
            QElapsedTimer lpT; lpT.start();
            poolPage = FPDF_LoadPage(m_poolHandle, m_req.pageIndex);
            const qint64 lpMs = lpT.elapsed();
            if (poolPage)
                PdfCloseTrace::openPage(m_poolHandle, poolPage, m_req.pageIndex,
                                        "ProgressiveRenderTask::run[pool]");
            lkLoad.unlock();
            if (lpMs >= 50)
                qDebug().noquote() << "[lat] LOADPAGE trang=" << m_req.pageIndex
                                   << "ms=" << lpMs;
        }
        if (!poolPage) {
            qDebug() << "[perf] drop page=" << m_req.pageIndex << "reason=poolLoadPageFailed";
            // 🔴 LƯỢT 12 — THỨ TỰ KHOÁ: `returnPoolHandle` lấy `m_handlePoolMutex`,
            // còn `initHandlePool` (:1170) giữ `m_handlePoolMutex` RỒI mới xin
            // `s_pdfiumMutex`. Gọi trả handle khi còn giữ khoá PDFium là AB-BA ⇒
            // hai luồng kẹt nhau, treo app. Nhả khoá PDFium TRƯỚC rồi mới trả handle.
            m_renderer->returnPoolHandle(m_poolHandle);
            emit finished(m_req.pageIndex, QImage());
            return;
        }
        // 🔴 LƯỢT 33e (mục 5): khe giữa hai vùng khoá — mở doc khẩn đang chờ ⇒
        // nhường ở đây (LoadPage đã xong, không phải làm lại). Trần 2 s chống đói.
        // 🔴 LƯỢT 33f (mục 1 — reviewer): UrgentPdfiumScope của CHÍNH task này (nếu là
        // trang hiển thị) đang sống ở đây ⇒ pdfiumUrgentPending() luôn true ⇒ khe ngủ
        // trọn 2 s mỗi lượt (hồi quy 33e). Trừ phần của mình: khẩn KHÁC mới nhường.
        { int gKhe = 0;
          while (gKhe++ < 100 && pdfiumUrgentPendingBeyond(laTrangHienThi ? 1 : 0)
                 && m_genRef->loadRelaxed() == m_req.generation)
              QThread::msleep(20); }
        MaybePdfiumLock lkPoolStart(__FILE__, __LINE__, trPoolLockOn(), m_req.pageIndex);
        QElapsedTimer startT; startT.start();   // LƯỢT 23 (DO): giu cua lat Start

        if (m_req.hideOwnAnnots)
            _ahGuard = std::make_unique<OwnAnnotHideGuard>(poolPage, true, true);

        double w = FPDF_GetPageWidth(poolPage);
        double h = FPDF_GetPageHeight(poolPage);
        if (m_pdfDoc) m_pdfDoc->updatePageSize(m_req.pageIndex, w, h);
        if (m_pdfDoc) m_pdfDoc->updatePageBoxOrigin(m_req.pageIndex, pdfBoxOrigin(poolPage));

        // V1b 0901: do so object TRUOC khi chon tran (xem PageRenderTask::run — cùng một lỗi
        // long-khoa: emit cuoi chi chay khi ve xong, trang 4000px khong bao gio xong).
        // 0927 LƯỢT 10 (--no-precount): BO khoi dem o ca nhanh POOL handle.
        if (!trNoPreCount()) { QElapsedTimer ct; ct.start();
          const int nObj = FPDFPage_CountObjects(poolPage);
          if (!m_renderer->isClosing())   // L33h: đừng ghi hash của renderer đang đóng (luồng nền)
            m_renderer->setPageObjectCount(m_req.pageIndex, nObj);
          qDebug() << "[heavycap] pre-count page=" << m_req.pageIndex << "objects=" << nObj
                   << "ms=" << ct.elapsed();
          armHeavyThumbYield(m_req.pageIndex, nObj); }   // 0928 LƯỢT 23

        double longSide = qMax(w, h);
        double maxPx;
        if (m_req.fullQuality && m_req.useZoomScale) {
            maxPx = m_renderer->fullQCapPx(m_req.pageIndex, longSide);
            qDebug().noquote() << "[dem] page=" << m_req.pageIndex << "render DAY DU"
                               << int(maxPx) << "px (hien thi thu nho ve"
                               << PdfRenderer::contDpi() << "DPI)";
        } else if (m_req.fullQuality) {
            maxPx = m_renderer->fullQCapPx(m_req.pageIndex, longSide);
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
        // 0928 LƯỢT 26 (VIỆC 1): TOAN bo tham so dau vao Start — so lan 1 vs lan 2
        // (page cache giu trang da parse? flags/scale/bitmap khac nhau?). 1 dong/luot.
        qDebug().noquote() << "[startargs] pool=1 page=" << m_req.pageIndex
                           << "bmp=" << m_bmpW << "x" << m_bmpH << "scale=" << m_renderScale
                           << "rot=0 l=0 t=0 flags=" << rflags
                           << "annot=" << m_req.renderAnnotations
                           << "hideOwn=" << m_req.hideOwnAnnots
                           << "fullQ=" << m_req.fullQuality << "zoomSc=" << m_req.useZoomScale
                           << "nObj=" << m_renderer->pageObjectCount(m_req.pageIndex);
        startStatus = FPDF_RenderPageBitmap_Start(m_bmp, poolPage, 0, 0,
                                                   m_bmpW, m_bmpH, 0,
                                                   rflags,
                                                   &pause); }

        m_renderStatus = startStatus;
        m_fpdfPage = poolPage;  // Lưu page để dùng trong loop Continue
        if (startStatus == FPDF_RENDER_TOBECONTINUED || startStatus == FPDF_RENDER_DONE)
            PdfCloseTrace::renderOpen(m_poolHandle, poolPage, "ProgressiveRenderTask::run[pool]");

        // 0927 LƯỢT 9: trả khoá trước vòng Continue (giống nhánh PageCache) — lát
        // Start đã xong, giao diện không phải chờ cả trang.
        lkPoolStart.unlock();
        // 0928 LƯỢT 23 (DO): lat Start = i=0, cùng khuôn [lat] de cong don Σkiem.
        if (latLogOn()) qDebug().noquote() << "[lat] trang=" << m_req.pageIndex << "i=0"
                           << "giu=" << startT.elapsed() << "cho=-1 kiem=" << pctx.checks;
        long long kiemCum = pctx.checks;

        // Emit first partial after Start
        if (m_renderStatus == FPDF_RENDER_TOBECONTINUED || m_renderStatus == FPDF_RENDER_DONE) {
            QImage partial = m_image.copy();
            emit pagePartial(m_req.pageIndex, scaleFactor(), partial);
            qDebug() << "[perf] slice ms=" << renderTimer.elapsed() << "page=" << m_req.pageIndex;
        }

        // ── Loop Continue với pool handle (KHÔNG cần khoá) ──
        m_lastEmitTimer.start();
        // 0928 LƯỢT 23 (DO): dem lat + do rieng thoi gian CHO KHOÁ va GIU KHOÁ cua
        // tung lat, cùng số ranh giới object PDFium đã vượt (`kiem`). Trả lời dứt
        // điểm: lap 2 chậm vì (a) phải chờ khoá dày hơn, hay (b) mỗi lat ăn ít
        // object hơn (việc cũ làm lại / cache toàn cục hỏng).
        int latIdx = 0;
        while (m_renderStatus == FPDF_RENDER_TOBECONTINUED) {
            if (m_genRef->loadRelaxed() != m_req.generation) {
                qDebug() << "[perf] drop page=" << m_req.pageIndex << "reason=genMismatchMid progressive";
                break;
            }
            // 🔴 LƯỢT 33b (J): tab NỀN dừng ở RANH GIỚI LÁT. Đo r31b/r33b: render trang
            // quái vật của tab nền chạy tiếp 3,7 s lát 50 ms sau khi tab khác được mở,
            // mỗi lát giành s_pdfiumMutex ⇒ tab mới chết đói. Check nay cat no ngay
            // lat ke tiep khi doc khong con la tab hien hanh.
            if (m_pdfDoc) {
                const FPDF_DOCUMENT a = PageCache::activeDoc();
                if (a && m_pdfDoc->raw() != a) {
                    qDebug() << "[perf] drop page=" << m_req.pageIndex << "reason=tab-nen-mid";
                    break;
                }
            }

            ProgressivePauseCtx pctx2;
            IFSDK_PAUSE pause2;
            pause2.version = 1;
            pause2.NeedToPauseNow = ProgressiveNeedToPauseNow;
            pause2.user = &pctx2;

            // 0927 LƯỢT 9: khoá chỉ bọc lệnh Continue, ngoài khối này KHÔNG khoá
            // ⇒ vẫn nhả khoá giữa các lát cho luồng giao diện và cho các task khác.
            qint64 choMs = 0, giuMs = 0; int kiem = 0;
            {
                QElapsedTimer latT; latT.start();
                MaybePdfiumLock lkPoolCont(__FILE__, __LINE__, trPoolLockOn(), m_req.pageIndex);
                choMs = latT.elapsed();                                  // thời gian xếp hàng
                // 🔴 LƯỢT 23 — GỐC CỦA "LAP 2 PAUSE DÀY HƠN". Đồng hồ lát phải chạy
                // TỪ LÚC VÀO KHOÁ, không phải từ lúc xin khoá. Bản cũ start() trước
                // khi khoá: mỗi lát chờ 40 ms (thumbnail giành) thì vào PDFium là
                // kim đã quá 50 ms ⇒ NeedToPauseNow trả lời ĐÚNG tại ranh giới object
                // kế tiếp ⇒ lát chỉ làm ~4 ms việc rồi nhả khoá, và cứ thế 455 lát
                // cho việc mà 166 lát làm xong (đo r23: lap2 giu_sum 8,1 s việc /
                // cho_sum 19,1 s chờ, kiem 1/lát; lap1 kiem 281/lát). Nhánh PageCache
                // (:710) đã đúng từ đầu — chỉ nhánh pool sai.
                pctx2.timer.start();
                m_renderStatus = FPDF_RenderPage_Continue(poolPage, &pause2);
                giuMs = latT.elapsed() - choMs;                          // thời gian trong khoá
                kiem = pctx2.checks;
            }
            cpuAcc.snap();                                               // L26: CPU sau moi lat
            ++latIdx;
            kiemCum += kiem;
            if (latLogOn()) qDebug().noquote() << "[lat] trang=" << m_req.pageIndex << "i=" << latIdx
                               << "giu=" << giuMs << "cho=" << choMs
                               << "kiem=" << kiem << "tong=" << kiemCum
                               << "lyDo=" << (giuMs >= pctx2.sliceMs ? "timeout" : "het-viec");

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
            // 🔴 0927 LƯỢT 8 — bắt buộc giữ `s_pdfiumMutex` cho CẢ BA lệnh PDFium dưới
            // đây. Bản `ProgressiveRenderTask::Close()` (dùng PageCache) đã khoá từ
            // trước; bản pool thì chạy lock-free vì vòng Continue cần nhả khoá, và hậu quả
            // là `FPDF_ClosePage` ở đây chạy SONG SONG với `PageCache::closeEntry` trên
            // luồng UI (qua `TextSelection::closeDocument`) — hai lệnh huỷ trang của hai
            // FPDF_DOCUMENT khác nhau nhưng CÙNG buffer mmap ⇒ bộ đệm stream/object dùng
            // chung bị thả quá tay ⇒ `CFX_RetainablePtr::Reset()` thấy refcount 0 ⇒ int3
            // tại RVA 0x1754b. Bằng chứng: crash8_quaivat.log dòng #000036 in `PAGE-CLOSE`
            // (tid=1ef8) mà không có `PAGE-CLOSE-XONG`.
            TimedMutexLocker lockClose(s_pdfiumMutex, "ProgressiveRenderTask::Close[pool]");
            if (m_renderStatus == FPDF_RENDER_TOBECONTINUED || m_renderStatus == FPDF_RENDER_DONE) {
                PdfCloseTrace::renderClose(m_poolHandle, poolPage, "ProgressiveRenderTask::run[pool]");
                FPDF_RenderPage_Close(poolPage);
            }
            if (m_bmp) { FPDFBitmap_Destroy(m_bmp); m_bmp = nullptr; }

            _ahGuard.reset();

            int objCount = FPDFPage_CountObjects(poolPage);
            emit pageObjectCount(m_req.pageIndex, objCount);

            PdfCloseTrace::closePage(m_poolHandle, poolPage,
                                     "ProgressiveRenderTask::run[pool]");
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
        cpuAcc.snap();
        qDebug().noquote() << "[latcpu] page=" << m_req.pageIndex
                           << "wall=" << renderTimer.elapsed() << "cpu=" << cpuAcc.cpuMs
                           << "lat=" << latIdx << "kiem=" << kiemCum
                           << "prio=" << ThreadCpuAcc::prio() << "pool=1";
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

    // VIỆC B: Pool rỗi — dùng đường cũ (s_pdfiumMutex), NHA KHOA giua cac lat.
    //
    // 🔴 0902 — BO GIU KHOA SUOT ("ProgressiveRenderTask::GiuSuot" 0901): no om
    // s_pdfiumMutex 2,7-2,9 gi moi luot ve trang vua sua chu thich; luong giao dien
    // xin khoa kieu chan o MainWindow:1835 bi chan theo thanh 7,1 gi, chu thich xep
    // hang khong hien (log 02/09). Chot bao ve chu thich lai giết hien thi chu thich.
    // 🔴🔴 VAN GIU YEU CAU CUA VA CHONG SAP 2026-09-01: sau khi nha khoa, luong giao
    // dien co the sua tai lieu ngay giua hai lat ⇒ tiep tuc FPDF_RenderPage_Continue
    // tren trang da bi sua sinh PDFium tu bung loi (0x80000003) hong heap (0xc0000374).
    // ⇒ Tinh nhat quan gi bang THE HE ANNOT, khong bang khoa doc quyen: moi luot
    //   them/xoa/sua chu thich tang PageCache::annotGeneration DUNG TRONG LUC no giu
    //   s_pdfiumMutex (bumpAnnotGeneration — xem AnnotationManager.cpp). Kiem tra the
    //   he DUOI KHOA truoc moi lat nen KHONG CON KHE HO: hoac ta thay bump truoc khi
    //   Continue (=> huy luot ve), hoac luot sua phai cho ta xong lat roi moi lay
    //   duoc khoa. Huy luot ve an toan vi cung luot sua da invalidatePage + dat luot
    //   ve moi (annotationAdded/pageContentChanged) — chu thich KHONG mat, chi hien
    //   o luot sau. Duong pool handle (phia tren) doc tai lieu rieng nen khong lien quan.
    FPDF_DOCUMENT annotDoc = m_pdfDoc ? m_pdfDoc->raw() : nullptr;
    quint64 annotGen0 = 0;
    bool annotStale = false;
    {
        TimedMutexLocker lockStart(s_pdfiumMutex, "ProgressiveRenderTask::Start");
        if (m_genRef->loadRelaxed() != m_req.generation) {
            qDebug() << "[perf] drop page=" << m_req.pageIndex << "reason=genMismatchPreStart";
            emit finished(m_req.pageIndex, QImage()); return;
        }

        m_fpdfPage = m_renderer->acquirePage(m_req.pageIndex);
        if (!m_fpdfPage) { emit finished(m_req.pageIndex, QImage()); return; }
        annotGen0 = PageCache::annotGeneration(annotDoc, m_req.pageIndex);  // moc the he DUOI KHOA

        if (m_req.hideOwnAnnots)
            _ahGuard = std::make_unique<OwnAnnotHideGuard>(m_fpdfPage, true, true);

        double w = FPDF_GetPageWidth(m_fpdfPage);
        double h = FPDF_GetPageHeight(m_fpdfPage);
        if (m_pdfDoc) m_pdfDoc->updatePageSize(m_req.pageIndex, w, h);
        if (m_pdfDoc) m_pdfDoc->updatePageBoxOrigin(m_req.pageIndex, pdfBoxOrigin(m_fpdfPage));

        // V1b 0901: do so object TRUOC khi chon tran (xem PageRenderTask::run — cùng một lỗi
        // long-khoa: emit cuoi chi chay khi ve xong, trang 4000px khong bao gio xong).
        // 0927 LƯỢT 10 (--no-precount): BO khoi dem o ca nhanh PageCache.
        if (!trNoPreCount()) { QElapsedTimer ct; ct.start();
          const int nObj = FPDFPage_CountObjects(m_fpdfPage);
          if (!m_renderer->isClosing())   // L33h: đừng ghi hash của renderer đang đóng (luồng nền)
            m_renderer->setPageObjectCount(m_req.pageIndex, nObj);
          qDebug() << "[heavycap] pre-count page=" << m_req.pageIndex << "objects=" << nObj
                   << "ms=" << ct.elapsed();
          armHeavyThumbYield(m_req.pageIndex, nObj); }   // 0928 LƯỢT 23

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
            maxPx = m_renderer->fullQCapPx(m_req.pageIndex, longSide);
            qDebug().noquote() << "[dem] page=" << m_req.pageIndex << "render DAY DU"
                               << int(maxPx) << "px (hien thi thu nho ve"
                               << PdfRenderer::contDpi() << "DPI)";
        } else if (m_req.fullQuality) {
            maxPx = m_renderer->fullQCapPx(m_req.pageIndex, longSide);
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
        // 0928 LƯỢT 26 (VIỆC 1): xem nhanh pool=1 ở tren — cung khuon, 1 dong/luot.
        qDebug().noquote() << "[startargs] pool=0 page=" << m_req.pageIndex
                           << "bmp=" << m_bmpW << "x" << m_bmpH << "scale=" << m_renderScale
                           << "rot=0 l=0 t=0 flags=" << rflags
                           << "annot=" << m_req.renderAnnotations
                           << "hideOwn=" << m_req.hideOwnAnnots
                           << "fullQ=" << m_req.fullQuality << "zoomSc=" << m_req.useZoomScale
                           << "nObj=" << m_renderer->pageObjectCount(m_req.pageIndex);
        m_renderStatus = FPDF_RenderPageBitmap_Start(m_bmp, m_fpdfPage, 0, 0,
                                                       m_bmpW, m_bmpH, 0,
                                                       rflags,
                                                       &pause); }
        if (m_renderStatus == FPDF_RENDER_TOBECONTINUED || m_renderStatus == FPDF_RENDER_DONE)
            PdfCloseTrace::renderOpen(m_pdfDoc ? m_pdfDoc->raw() : nullptr, m_fpdfPage,
                                     "ProgressiveRenderTask::run[PageCache]");
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
    // 0928 LƯỢT 26 (VIỆC 1): dem lat + kiem cho [latcpu] nhanh — nhu nhanh pool.
    int latIdx2 = 0; long long kiemCum2 = 0;
    while (m_renderStatus == FPDF_RENDER_TOBECONTINUED) {
        // Check generation before each slice — cancel if stale
        if (m_genRef->loadRelaxed() != m_req.generation) {
            qDebug() << "[perf] drop page=" << m_req.pageIndex << "reason=genMismatchMid progressive";
            break;
        }
        // 🔴 LƯỢT 33b (J): tab NỀN dừng ở ranh giới lát (xem nhanh pool ben tren).
        if (m_pdfDoc) {
            const FPDF_DOCUMENT a = PageCache::activeDoc();
            if (a && m_pdfDoc->raw() != a) {
                qDebug() << "[perf] drop page=" << m_req.pageIndex << "reason=tab-nen-mid";
                break;
            }
        }

        ProgressivePauseCtx pctx;
        {
            TimedMutexLocker lockCont(s_pdfiumMutex, "ProgressiveRenderTask::Continue");
            // Kiem the he DUOI KHOA, TRUOC khi tiep tuc lat ve: co luot sua chu thich
            // xen giua hai lat => KHONG Continue tren trang da bi sua (nguon crash
            // 0x80000003 ngay 01/09) — huy luot ve, luot moi da duoc ben sua dat.
            if (PageCache::annotGeneration(annotDoc, m_req.pageIndex) != annotGen0) {
                annotStale = true;
                qDebug().noquote() << "[perf] drop page=" << m_req.pageIndex
                                   << "reason=annotGenMid — huy luot ve, khong ve tiep tren trang da sua";
                break;
            }
            pctx.timer.start();
            IFSDK_PAUSE pause;
            pause.version = 1;
            pause.NeedToPauseNow = ProgressiveNeedToPauseNow;
            pause.user = &pctx;

            m_renderStatus = FPDF_RenderPage_Continue(m_fpdfPage, &pause);
            // Mutex unlocked here — other threads can use PDFium between slices
        }
        cpuAcc.snap();
        ++latIdx2; kiemCum2 += pctx.checks;

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
    bool cancelled = (m_genRef->loadRelaxed() != m_req.generation) || annotStale;

    // Emit one final partial if Done
    if (m_renderStatus == FPDF_RENDER_DONE && !cancelled) {
        QImage finalPartial = m_image.copy();
        emit pagePartial(m_req.pageIndex, scaleFactor(), finalPartial);
    }

    {
        TimedMutexLocker lockClose(s_pdfiumMutex, "ProgressiveRenderTask::Close");
        if (m_renderStatus == FPDF_RENDER_TOBECONTINUED || m_renderStatus == FPDF_RENDER_DONE) {
            PdfCloseTrace::renderClose(m_pdfDoc ? m_pdfDoc->raw() : nullptr, m_fpdfPage,
                                       "ProgressiveRenderTask::run[PageCache]");
            FPDF_RenderPage_Close(m_fpdfPage);
        }
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

    cpuAcc.snap();
    qDebug().noquote() << "[latcpu] page=" << m_req.pageIndex
                       << "wall=" << renderTimer.elapsed() << "cpu=" << cpuAcc.cpuMs
                       << "lat=" << latIdx2 << "kiem=" << kiemCum2
                       << "prio=" << ThreadCpuAcc::prio() << "pool=0";
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
    // 🔴 0928 LƯỢT 22: token lúc SPAWN (xem PageRenderTask).
    , m_task(m_pdfDoc ? m_pdfDoc->raw() : nullptr, "RegionRenderTask")
{
    setAutoDelete(true);
    connect(this, &RegionRenderTask::finished, receiver,
            [](int, double, QRect, QImage){}, Qt::QueuedConnection);
}

void RegionRenderTask::run() {
    // 0928 LƯỢT 14: xem PageRenderTask::run(). LƯỢT 22: m_task (ctor đăng ký).
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
        if (m_renderStatus == FPDF_RENDER_TOBECONTINUED || m_renderStatus == FPDF_RENDER_DONE)
            PdfCloseTrace::renderOpen(m_pdfDoc ? m_pdfDoc->raw() : nullptr, m_fpdfPage,
                                     "RegionRenderTask::run[PageCache]");
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
        if (m_renderStatus == FPDF_RENDER_TOBECONTINUED || m_renderStatus == FPDF_RENDER_DONE) {
            PdfCloseTrace::renderClose(m_pdfDoc ? m_pdfDoc->raw() : nullptr, m_fpdfPage,
                                       "RegionRenderTask::run[PageCache]");
            FPDF_RenderPage_Close(m_fpdfPage);
        }
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

        // 🔴 0927 LƯỢT 2: `ffi` TRƯỚC khai báo TRONG khoi `if (hasForms)` nên nó chết
        // ở dấu `}` bên dưới, TRƯỚC khi FPDF_FFLDraw dùng nó ở vòng for và trước
        // FPDFDOC_ExitFormFillEnvironment — trái hợp đồng "formInfo must remain
        // valid until the FPDF_FORMHANDLE is closed" (fpdf_formfill.h). Nay dung
        // FormEnv (RAII, `ffi` o HAM) ⇒ còn sống tới hết handle + luôn OnBeforeClosePage.
        FormEnv _form;
        _form.open(m_pdfDoc ? m_pdfDoc->raw() : nullptr, page, "TileBatchRenderTask::run");

        for (const QPoint& tile : m_tiles) {
            if (m_genRef->loadRelaxed() != m_reqGen) {
                _form.close();
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
            if (_form.form) {
                FPDF_FFLDraw(_form.form, bmp, page,
                             -col * kTileSize, -row * kTileSize,
                             fullW, fullH, 0, FPDF_ANNOT);
            }
            FPDFBitmap_Destroy(bmp);

            emit tileDone(m_pageIndex, m_scale, col, row, std::move(image));
        }

        _form.close();
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
    // GIA DINH caller giu s_pdfiumMutex (nhu acquirePage). Cap doi bat buoc.
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

// 🔴 LƯỢT 30 (VIỆC 1 — DO, nghi phạm A): đếm slot pool ĐÃ TỪNG vẽ (mỗi cái giữ
// object cache của các trang nó vẽ cho tới khi doc đóng).
QString PdfRenderer::probePoolInfo() const {
    QMutexLocker lk(&m_handlePoolMutex);
    int nSlots = 0, ever = 0, idle = 0;
    long long renders = 0;
    for (const auto& s : m_handlePool) {
        if (!s.doc) continue;
        ++nSlots;
        if (s.everUsed) ++ever;
        if (!s.inUse)   ++idle;
        renders += s.renders;
    }
    return QStringLiteral("slots=%1 everUsed=%2 idle=%3 renders=%4")
        .arg(nSlots).arg(ever).arg(idle).arg(renders);
}

int PdfRenderer::probeIdlePoolCount() const {
    QMutexLocker lk(&m_handlePoolMutex);
    int idle = 0;
    for (const auto& s : m_handlePool)
        if (s.doc && !s.inUse) ++idle;
    return idle;
}

// Đóng MỘT doc pool RẢNH (không ai mượn) — cùng thứ tự/đếm như closeHandlePool.
// Caller đo commit truoc/sau de biet doc nay giu bao nhieu bo nho (nghi phạm A).
bool PdfRenderer::probeCloseOneIdlePoolDoc() {
    QMutexLocker lock(&m_handlePoolMutex);
    for (auto& slot : m_handlePool) {
        if (!slot.doc || slot.inUse) continue;
        PdfCloseTrace::docCloseBegin(slot.doc, "PdfRenderer::probeCloseOneIdlePoolDoc");
        {
            BoundedPdfiumLock plock(__FILE__, __LINE__);
            FPDF_CloseDocument(slot.doc); g_pdfiumPoolClose.fetchAndAddOrdered(1);
        }
        PdfCloseTrace::docCloseDone(slot.doc, "PdfRenderer::probeCloseOneIdlePoolDoc");
        slot.doc = nullptr;
        return true;
    }
    return false;
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
    // 🔴 0927 LƯỢT 8 — ĐÚNG THỨ TỰ, dùng chung với setDocument() và với
    // MainWindow::shutdownTab() (đóng tab / thoát app):
    //   ① HUỶ (bump generation) → task cắt ở slice kế tiếp
    //   ② CHỜ pool có hạn   → task trả FPDF_PAGE + trả pool handle
    //   ③ đóng doc của POOL → FPDF_CloseDocument ×12
    // Trước đây (lượt 2) là `m_generation += 999` rồi `waitForDone()` VÔ HẠN, và
    // `closeHandlePool()` chạy khi task còn sống (POOL-SKIP). `setDocument()` lại
    // `waitForDone()` vô hạn. Cả hai đều treo 14 s với trang CAD nặng.
    s_globalCacheBytes.fetch_sub(m_cacheBytes);
    m_cacheBytes = 0;
    // 🔴🔴 0929 LƯỢT 33h: phần nặng (①②③) đã chạy trên LUỒNG NỀN qua shutdownHeavy()
    // (closeJob ở MainWindow::onTabClose) ⇒ ở đây là no-op (guard). Nếu ~PdfRenderer
    // chạy trên UI (thoát app) thì shutdownHeavy() làm nốt tại đây — đúng luồng.
    shutdownHeavy();
    if (s_renderCount.load() > 0)
        qDebug() << "[Renderer] total FPDF_RenderPageBitmap calls:" << s_renderCount.load();
}

// 🔴🔴 0929 LƯỢT 33h (dump r33g, ACCESS VIOLATION đọc 0x8 ở setPageObjectCount):
// PdfRenderer là QObject affinity LUỒNG UI nhưng bị `delete` trên luồng nền (closeJob
// `delete t`). Trong lúc ~QObject chạy ở nền, ProgressiveRenderTask vẫn phát
// pageObjectCount ⇒ QMetaCallEvent được UI giao cho renderer đang chết ⇒ UAF.
// SỬA GỐC: nền chỉ làm phần nặng KHÔNG đụng máy móc QObject (huỷ + chờ pool + đóng
// 12 doc pool + orphan); renderer sau đó bị `delete` TRÊN UI. Hàm này chỉ chạm
// m_generation/m_pending*/m_mainPool/m_thumbPool/m_handlePool* — toàn atomic/mutex/
// QThreadPool, KHÔNG đụng event queue ⇒ an toàn khi gọi từ luồng nền. Idempotent.
void PdfRenderer::shutdownHeavy() {
    if (m_heavyShutdown) return;
    m_heavyShutdown = true;
    cancelPending();                            // ①
    waitIdle(kPoolWaitMs);                      // ②
    closeHandlePool();                          // ③
    // Lưới an toàn: slot nào lúc `closeHandlePool` còn `inUse` thì handle được giữ ở
    // `m_orphanPoolDocs` (KHÔNG quăng bỏ như bản lượt 2 — bỏ rơi thì 12 doc sống mãi
    // trên buffer đã unmap). Chỉ còn đường này nếu `waitIdle` hết giờ.
    if (!m_orphanPoolDocs.isEmpty()) {
        BoundedPdfiumLock plock(__FILE__, __LINE__);
        while (!m_orphanPoolDocs.isEmpty()) {
            FPDF_DOCUMENT d = m_orphanPoolDocs.takeFirst();
            PdfCloseTrace::docCloseBegin(d, "~PdfRenderer::orphanPool");
            FPDF_CloseDocument(d); g_pdfiumPoolClose.fetchAndAddOrdered(1);
            PdfCloseTrace::docCloseDone(d, "~PdfRenderer::orphanPool");
        }
    }
}

// 🔴🔴 0928 LƯỢT 22b (reviewer mục 2) — LUỒNG UI, trước `delete t` xuống nền.
// ~QObject xoá posted events của từng object nó huỷ — nhưng chỉ HỢP LỆ khi việc
// huỷ đó xảy ra trên luồng sở hữu queue (UI). ở nền thì nó sửa queue của UI trong
// lúc UI đang giao event ⇒ race/UAF. Nên dọn hết NGAY ĐÂY, trên UI:
//   ① cờ closing: các site spawn watcher/invokeMethod Queued dừng lại;
//   ② delete watcher con (UI thread — đúng chủ queue, ~QObject tự gỡ event của nó);
//   ③ removePostedEvents(this): QMetaCallEvent của invokeMethod Queued (1847/1871)
//      và các lambda queued còn nằm trong hàng đợi UI.
// Không chờ future writePage: huỷ watcher chỉ detach interface (Qt đảm bảo), task
// ghi đĩa tự xong rồi bỏ kết quả — không đụng renderer nữa sau ③.
void PdfRenderer::prepareForClose() {
    m_closing.store(true);
    qDeleteAll(findChildren<QFutureWatcherBase*>());
    QCoreApplication::removePostedEvents(this);
}

void PdfRenderer::setDocument(PdfDocument* doc) {
    cancelPending();
    waitIdle(kPoolWaitMs);
    closeHandlePool();
    m_doc = doc;
    clearCache();
    m_pageObjectCount.clear();
    m_pageAnnotRender.clear();
    m_pageAnnotOverlay.clear();
    // 🔴 LƯỢT 33e (muc 5 — freeze J 1766 ms): setDocument chay tren GUI KHONG con
    // dung handle pool nua — initHandlePool = 3 lan FPDF_LoadMemDocument xep hang
    // sau trang quai vat giu khoa ⇒ UI dong 1,7 s (do r33e: "bounded-gui blocked
    // at :1527" + freeze_max step=J=1766). Worker (borrowPoolHandle) tu dung lazy.
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
    // 🔴 LƯỢT 33e: KHONG init tai day — worker xin handle se tu dung (lazy),
    // GUI khong FPDF_LoadMemDocument ×3 nua (cùng nguyên nhân freeze J).
}

// 0928 LƯỢT 24 — A/B cho CEO: TORREADER_POOL_SIZE đọc MỘT lần lúc khởi động,
// kẹp 1..12.
// 🔴 LƯỢT 30 (VIỆC 2, theo SỐ): --ram-probe đo pool 12 vs 3 vs 1 chỉ khác nhau 8%
// commit (4 698 / 4 456 / 4 334 MB) vì KHOÁ CHUNG s_pdfiumMutex serialise mọi lượt vẽ
// ⇒ chỉ ~2 handle THỰC SỰ vẽ, 10 cái kia chỉ giữ xref. L24 đã đo "12 vs 3 cùng tốc độ".
// ⇒ Hạ mặc định 12 → 3: bớt ~9 FPDF_DOCUMENT vô dụng/tab, không chậm hơn.
static int poolSize() {
    static const int v = [] {
        bool ok = false;
        const int n = qEnvironmentVariableIntValue("TORREADER_POOL_SIZE", &ok);
        const int bound = ok ? qBound(1, n, 12) : 3;
        qDebug().noquote() << "[pool] size=" << bound << (ok ? "(env)" : "(mac dinh 3)");
        return bound;
    }();
    return v;
}

void PdfRenderer::initHandlePool() {
    if (!m_doc || !m_doc->isOpen() || !m_doc->hasMmap()) {
        qDebug() << "[song song] pool SKIP — no mmap available";
        return;
    }
    QMutexLocker lock(&m_handlePoolMutex);
    // 🔴 LƯỢT 33e (lazy init): worker A vừa dựng pool + đang giữ handle ⇒ worker B
    // KHÔNG được clear() rồi dựng lại (hủy document A đang vẽ giữa chứng).
    if (!m_handlePool.empty()) return;
    m_handlePool.clear();

    // Mở poolSize() handle bổ sung từ cùng buffer memory-mapped
    for (int i = 0; i < poolSize(); ++i) {
        BoundedPdfiumLock plock(__FILE__, __LINE__);
        FPDF_DOCUMENT poolDoc = FPDF_LoadMemDocument(
            m_doc->mmapData(),
            static_cast<int>(m_doc->mmapSize()),
            m_doc->password()
        ); if (poolDoc) g_pdfiumPoolOpen.fetchAndAddOrdered(1);
        if (poolDoc) {
            PdfCloseTrace::docOpen(poolDoc, "PdfRenderer::initHandlePool");
            m_handlePool.push_back({poolDoc, false});
            qDebug() << "[song song] pool INIT slot=" << i;
        } else {
            qDebug() << "[song song] pool FAIL open slot=" << i;
        }
    }
}

void PdfRenderer::closeHandlePool() {
    QMutexLocker lock(&m_handlePoolMutex);
    int skipped = 0;
    for (auto& slot : m_handlePool) {
        if (!slot.doc) continue;
        if (slot.inUse) {
            // 0927: co nguoi VAN MUON handle nay. FPDF_CloseDocument luc nay se pha
            // trang no dang mo ⇒ CHECK 0x80000003 trong pdfium. Giu lai (ro doc do
            // bi "phinh" khi he ket thuc — an toan hon CHECK) va bao loi de biet
            // duong nao goi closeHandlePool() khi luong render con chay.
            PdfCloseTrace::note("POOL-SKIP",
                QStringLiteral("doc=%1 (dang muon — KHONG dong)").arg(reinterpret_cast<quintptr>(slot.doc), 0, 16));
            ++skipped;
            continue;
        }
        PdfCloseTrace::docCloseBegin(slot.doc, "PdfRenderer::closeHandlePool");
        {
            BoundedPdfiumLock plock(__FILE__, __LINE__);
            FPDF_CloseDocument(slot.doc); g_pdfiumPoolClose.fetchAndAddOrdered(1);
        }
        PdfCloseTrace::docCloseDone(slot.doc, "PdfRenderer::closeHandlePool");
        slot.doc = nullptr;
    }
    // 🔴 0927 LƯỢT 2: ban "POOL-SKIP" truoc day ROI handle (continue) roi `clear()`
    // ⇒ FPDF_DOCUMENT do bi QUANG BO, KHONG bao gio FPDF_CloseDocument: 12 tai lieu
    // con no tren cung buffer mmap, va FPDF_DestroyLibrary gap CHECK. LUU lai,
    // ~PdfRenderer cho task ve het roi dong.
    for (auto& slot : m_handlePool)
        if (slot.doc) m_orphanPoolDocs.append(slot.doc);
    if (!m_orphanPoolDocs.isEmpty())
        PdfCloseTrace::note("POOL-ORPHAN",
            QStringLiteral("giu lai %1 tai lieu de dong lai o ~PdfRenderer").arg(m_orphanPoolDocs.size()));
    if (skipped)
        qWarning().noquote() << QStringLiteral(
            "[closeorder] POOL-CLOSE xong boQua=%1 — co task chua tra handle (xem POOL-SKIP)").arg(skipped);
    m_handlePool.clear();
}

bool PdfRenderer::hasPoolHandles() const {
    QMutexLocker lk(&m_handlePoolMutex);
    return !m_handlePool.empty();
}

FPDF_DOCUMENT PdfRenderer::borrowPoolHandle() {
    // 0927 LƯỢT 10 (--no-pool): KHONG cap handle pool. Day la PHAI DUY NHAT de
    // `ProgressiveRenderTask::run` (ProgressiveRenderTask.cpp ~340) va
    // `ThumbnailWorker::run` (qua borrowPoolHandleForThumbnail, :1265) luon rơi ve
    // DUONG CU: FPDF_LoadPage tren PageCache / tai lieu chinh DUOI s_pdfiumMutex.
    if (trNoPool()) return nullptr;
    auto timKiem = [this]() -> FPDF_DOCUMENT {
        QMutexLocker lock(&m_handlePoolMutex);
        for (auto& slot : m_handlePool) {
            if (slot.doc && !slot.inUse) {
                slot.inUse = true;
                slot.everUsed = true;              // 🔴 LƯỢT 30 (DO): doc này giờ giữ object cache
                ++slot.renders;
                qDebug() << "[song song] muon handle — available";
                return slot.doc;
            }
        }
        return nullptr;
    };
    if (FPDF_DOCUMENT h = timKiem()) return h;
    // 🔴 LƯỢT 33e: pool RỠNG (chưa dựng — setDocument GUI không còn init) ⇒
    // worker tự dựng ngay đây (load FPDF_LoadMemDocument ×N nằm trên luồng nền).
    {
        QMutexLocker lock(&m_handlePoolMutex);
        if (!m_handlePool.empty()) {
            qDebug() << "[song song] muon handle — pool empty, fallback to main doc";
            return nullptr;                        // pool đã dựng, chỉ hết slot rỗi
        }
    }
    initHandlePool();
    if (FPDF_DOCUMENT h2 = timKiem()) return h2;
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
        // 0928 LƯỢT 24 — kẹp han muc theo kich thuoc pool THUC (A/B POOL_SIZE): voi pool 3
        // ma quota 4 thi thumbnail an het, render trang mat duong song song; pool 1 thi
        // thumbnail khong muon gi ca. Mac dinh 12: min(4, 11) = 4 — KHONG doi.
        const int quota = qMin(kThumbHandleQuota, qMax(0, poolSize() - 1));
        QMutexLocker lock(&m_handlePoolMutex);
        if (m_thumbHandlesInUse >= quota) {
            qDebug() << "[thumb quota] du" << m_thumbHandlesInUse << "/" << quota
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

// 🔴 LƯỢT 31 (A3 + B5 — reviewer bắt): đuổi theo KHOẢNG CÁCH TỚI CỬA SỔ VIEWPORT,
// không chỉ m_currentPage. Ở Continuous nhiều trang cùng hiển; bản cũ chỉ bảo vệ
// ĐÚNG m_currentPage ⇒ các trang hiển khác có thể bị đuổi khỏi m_cache. Luật mới:
//   - BẢO VỆ mọi trang trong [viewport-2, viewport+2] (dù vượt trần — chúng đang
//     hiện/gần hiện, đuổi đi là re-render ngay ⇒ churn + đứng hình).
//   - Ngoài cửa sổ: đuổi trang XA cửa sổ nhất trước, tới khi về dưới trần.
//   - Single: view gọi setCurrentPage nên cửa sổ = [page,page] — giữ nguyên ý cũ.
void PdfRenderer::evictCache() {
    const qint64 budget = maxCacheBytes();
    if (s_globalCacheBytes.load() <= budget) return;
    int vpLo = m_vpFirst.load(std::memory_order_relaxed);
    int vpHi = m_vpLast.load(std::memory_order_relaxed);
    if (vpHi < vpLo) { vpLo = vpHi = m_currentPage; }   // chưa có window → trang hiện
    const int kVpMargin = 2;                            // ±2 trang quanh viewport
    const int protLo = vpLo - kVpMargin, protHi = vpHi + kVpMargin;
    auto distToVp = [&](int page) {
        if (page < vpLo) return vpLo - page;
        if (page > vpHi) return page - vpHi;
        return 0;
    };
    using KV = QPair<int, int>;  // distance, pageIndex
    QVector<KV> byDist;
    byDist.reserve(m_cache.size());
    for (auto it = m_cache.cbegin(); it != m_cache.cend(); ++it)
        byDist.append({distToVp(it.key()), it.key()});
    std::sort(byDist.begin(), byDist.end(),
              [](const KV& a, const KV& b) { return a.first > b.first; });
    // Mo tai lieu duoc giu TOI THIEU 3 trang de khong bi vet sach — duoi ap luc bo
    // nho cung con noi trang.
    const int kMinKeep = 3;
    for (const auto& kv : byDist) {
        const int page = kv.second;
        // TUYET DOI khong don trang DANG XEM / trong cua so bao ve: don no di thi
        // no render lai roi lai bi don, vong lap vo tan va man hinh khong bao gio net.
        if (page == m_currentPage) continue;
        if (page >= protLo && page <= protHi) continue;
        if (s_globalCacheBytes.load() <= budget || m_cache.size() <= kMinKeep) break;
        qint64 sz = static_cast<qint64>(m_cache[page].sizeInBytes());
        m_cacheBytes -= sz;
        s_globalCacheBytes.fetch_sub(sz);
        m_cache.remove(page);
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
                if (fullQuality && !m_closing.load() && m_tileCache && m_tileCache->isOpen() && !m_writingCache.contains(pageIndex)) {
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

            if (fullQuality && !m_closing.load() && m_tileCache && m_tileCache->isOpen() && !m_writingCache.contains(pageIndex)) {
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

    // Memory cache — accept any full-quality image (zoom-independent)
    if (m_cache.contains(pageIndex)) {
        const QImage& cached = m_cache[pageIndex];
        // 🔴 LƯỢT 31b (reviewer mục 2): phát scale THẬT của ảnh đang giữ, như nhánh
        // disk bên dưới. Đính nhãn fullQCapPx/longSide (= ~4000px khi mở lại mà
        // pageObjectCount=0) lên ảnh tạm 357px ⇒ ContinuousView tưởng đã nét, bỏ
        // vẽ lại ⇒ KẸT MỜ ở zoom lớn.
        const double memScale = qMax(cached.width(), cached.height()) / qMax(longSide, 1.0);
        qDebug() << "[perf] cache-only-for-cont hit mem page=" << pageIndex
                 << "renderedScale=" << memScale;
        emit continuousPageReady(pageIndex, cached, memScale);
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
        // 🔴 LƯỢT 31 (B6): so voi CANH DAI DA LUU (doc tu TorEntry.pad, ghi luc
        // writePage) — KHONG doan lai fullQCapPx tu pageObjectCount (= 0 luc MO LAI
        // ⇒ trang quái vật bị coi là NHẸ ⇒ đòi 4000px ⇒ VUT ảnh 357px ⇒ vẽ lại ~9 s).
        // Entry cu (stored=0) → fallback fullQCapPx nhu cu. San 64px van giu chot
        // chong anh 1x1/hong. ⇒ Mở lại dùng CÙNG trần px ⇒ hiện ngay từ cache.
        int expectLong = m_tileCache->storedLongPx(pageIndex, CacheZoom::Full);
        if (expectLong <= 0) expectLong = int(fullQCapPx(pageIndex, longSide));
        const int acceptMin = qMax(64, int(expectLong * 0.9));
        if (!cached.isNull() && cachedLong < acceptMin) {
            qWarning() << "[cache] VUT entry hong page=" << pageIndex
                       << "canhDai=" << cachedLong << "can>=" << acceptMin;
            cached = QImage();   // coi nhu MISS — luot render day du ke tiep se ghi de
        }
        if (!cached.isNull()) {
            // 🔴 LƯỢT 31 (B6): phát scale THẬT của ảnh đã lưu (canhDai/longSide),
            // không dùng `renderedScale` (= fullQCapPx/longSide — 4000/long khi mở
            // lại mà pageObjectCount=0) ⇒ ContinuousView ghi đúng m_pageImageZoom,
            // không tưởng ảnh 357px là 4000px rồi về sau tính lại sai.
            const double diskScale = double(cachedLong) / qMax(longSide, 1.0);
            qDebug() << "[perf] cache-only-for-cont hit disk page=" << pageIndex
                     << "renderedScale=" << diskScale << "canhDai=" << cachedLong;
            cacheInsert(pageIndex, cached);
            qDebug() << "[perf] cache add page=" << pageIndex
                     << "global=" << (s_globalCacheBytes.load() / 1048576) << "MB"
                     << "pages(tab)=" << m_cache.size();
            evictCache();
            emit continuousPageReady(pageIndex, cached, diskScale);
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

    // 0902: `scale` day = zoom * devicePixelRatio = kich thuoc hien thi THAT cua khung nhin
    // (chinh la so `canCo` in ra o "cont sharp request"). Bom vao renderer de fullQCapPx
    // cat tran trang nang theo PIXEL MAN HINH, khong theo DPI co dinh nua. Moi duong goi
    // (primary/neighbor/dequeue/retry) deu qua day => luon dung gia tri hien hanh.
    setContinuousDisplayScale(scale);

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
                             fullQCapPx(pageIndex, longSide) / qMax(longSide, 1.0));
    } else {
        renderedScale = fullQCapPx(pageIndex, longSide) / qMax(longSide, 1.0);
    }

    // Accept any full-quality cached image (all at kFullRenderMaxPx resolution)
    if (m_cache.contains(pageIndex)) {
        const QImage& cached = m_cache[pageIndex];
        // 🔴 LƯỢT 31b (reviewer mục 2): phát scale THẬT của ảnh trong cache. Nếu nó
        // thấp hơn độ phân giải cần (dung sai 0,9 — làm tròn px) thì ảnh cache CHỈ LÀ
        // ẢNH TẠM: phát nó làm placeholder rồi RƠI XUỐNG vẽ thật (dedup
        // m_contPageGen bên dưới chặn xếp chồng lượt đang bay).
        const double memScale = qMax(cached.width(), cached.height()) / qMax(longSide, 1.0);
        qDebug() << "[perf] cont cache hit mem page=" << pageIndex
                 << "renderedScale=" << memScale;
        emit continuousPageReady(pageIndex, cached, memScale);
        if (memScale >= renderedScale * 0.9)
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
            if (!m_closing.load())   // L22b: đừng post event Queued vào renderer đang đóng
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
                if (!m_closing.load())   // L22b: như trên — không post khi đang đóng tab
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
        // 🔴 LƯỢT 31b: THAY ảnh cũ bằng ảnh vừa vẽ (cacheInsert tự trừ size cũ). Bản cũ
        // `if (!contains)` giữ ảnh tạm 357px từ lần mở lại ⇒ hit mem kế tiếp phát lại
        // ảnh mờ đè lên ảnh net vừa về.
        cacheInsert(pageIndex, img);
        qDebug() << "[perf] cache add page=" << pageIndex
                 << "global=" << (s_globalCacheBytes.load() / 1048576) << "MB"
                 << "pages(tab)=" << m_cache.size();
        evictCache();

        // 🔴 CHOT 2026-08-31: View Fast render o scale zoom (vd 274px) — KHONG duoc ghi
        // vao o CacheZoom::Full, vi lan doc sau se tuong do la anh day du 4000px.
        // Day chinh la co che DAU DOC cache: mot lan render hong la hong VINH VIEN
        // tren dia, sua code cung khong cuu duoc cho toi khi xoa file cache.
        // 🔴 SUA 2026-08-31 (lan 2): truoc day so voi kFullRenderMaxPx*0.9 = 3600px. Sau khi
        // chuyen sang siet theo DPI thi o 75 DPI trang A0 chi 2480px => chot NAY CHAN SACH
        // viec ghi dem, moi lan mo lai phai render lai tu dau. Phai so voi KY VONG theo DPI.
        const int outLong = qMax(img.width(), img.height());
        const QSizeF fullSz = m_doc ? m_doc->pageSize(pageIndex) : QSizeF();
        // O Full chi nhan anh DAY DU theo tran cua chinh trang nay (fullQCapPx).
        const bool fullQualityImg = outLong >= int(fullQCapPx(pageIndex, qMax(fullSz.width(), fullSz.height())) * 0.9);
        if (!fullQualityImg)
            qDebug() << "[cache] KHONG ghi dia page=" << pageIndex
                     << "canhDai=" << outLong << "(chua du chat luong day du)";
        if (fullQualityImg && !m_closing.load() && m_tileCache && m_tileCache->isOpen() && !m_writingCache.contains(pageIndex)) {
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
        // 🔴 LƯỢT 31b: phát scale THẬT của ảnh vừa vẽ. `renderedScale` chốt lúc dispatch
        // (pageObjectCount có thể còn 0 → 4000px), còn task đã tự tính lại trần sau
        // pre-count (trang 2,18M object → ~1240px) ⇒ nhãn cũ phình to ảnh thật.
        const double doneLong = qMax(fullSz.width(), fullSz.height());
        const double doneScale = doneLong > 0 ? double(outLong) / doneLong : renderedScale;
        emit continuousPageReady(idx, img, doneScale);
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

void PdfRenderer::setCurrentPage(int page) {
    m_currentPage = page;
    // 0928: đánh dấu trang hiển thị — mọi việc PDFium của trang này được quyền
    // ưu tiên trên khoá chung, việc nền (thumbnail/OCR) phải nhường.
    m_urgentPage.store(page, std::memory_order_release);
    // 🔴 LƯỢT 31 (A3): Single — cửa sổ = đúng trang này (ContinuousView sẽ ghi đè
    // window thật khi ở chế độ Continuous).
    m_vpFirst.store(page, std::memory_order_relaxed);
    m_vpLast.store(page,  std::memory_order_relaxed);
}

void PdfRenderer::setTileCache(std::shared_ptr<TileCacheFile> cache) { m_tileCache = std::move(cache); }
