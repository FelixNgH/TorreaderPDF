#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "ThumbnailRenderPool.h"
#include "PageCache.h"
#include "PdfiumLock.h"
#include "DocTaskGate.h"
#include "Bisect.h"
#include "PdfRenderer.h"  // VIỆC: để gọi borrowPoolHandle/returnPoolHandle
#include <QMutexLocker>
#include <QElapsedTimer>
#include <QDateTime>
#include <QDebug>
#include <QFileInfo>
#include <QThread>
#include <fpdf_progressive.h>
#include <fpdf_thumbnail.h>

// Progressive-pause helpers (mirrors PdfRenderer.h pattern).
struct ThumbPauseCtx {
    QElapsedTimer timer;
    int sliceMs = 50;
};
static FPDF_BOOL ThumbNeedToPauseNow(IFSDK_PAUSE* pThis) {
    auto* ctx = static_cast<ThumbPauseCtx*>(pThis->user);
    return ctx->timer.elapsed() >= ctx->sliceMs;
}

// 0928 LUOT 13: treo toi da 400 vong (400 x 20 ms = 8 s) roi thi co len khong
// dung bi bo doi. Thumbnail la viec NEN — it nhat phai cho tiep, nhung khong cho
// vo han (thi thumbnail 350 trang khong bao gio len het).
static constexpr int kThumbUrgentMaxYields = 200;

// 🔴 LƯỢT 33e (mục 5 — J): cửa sổ "trang đầu đang chờ ảnh". MainWindow đặt
// g_pdfiumUrgentPage = 0 khi doc mở xong (trần 5 s), ContinuousView xóa lúc ACCEPT.
// Thumbnail phải NHƯỜNG cả trong cửa sổ này, không chỉ lúc scope mở doc — đo r33d:
// lượt vẽ trang 0 của f2 chờ 370 ms SAU khi doc đã mở, vì 350 ô thumbnail uuTien=2
// bao nhau cắt khoá 50–70 ms/ô.
static bool thumbUrgentWindow() {
    const int up = g_pdfiumUrgentPage().load(std::memory_order_acquire);
    return up >= 0 && QDateTime::currentMSecsSinceEpoch()
        <= g_pdfiumUrgentPageDeadline().load(std::memory_order_acquire);
}

// 🔴 0928 LƯỢT 15 — TRẦN THỜI GIAN cho MỘT trang thumbnail. Trang nặng (≥150k
// object) có thể nuốt hàng giây ở tỉ lệ 0.15, chặn cả hàng ⇒ các ô đang hiện
// phía sau nó trắng. Quá trần thì phát bản NHÁP (bitmap đã vẽ được tới đâu)
// cho ô đó rồi đẩy lại hàng ở tầng nền để sau này vẽ trọn. 5 s = nằm trong
// mục tiêu CEO "ô trang nặng ≤ 6 s", vẫn dư thời gian cho trang nhẹ (~0.2–0.4 s).
static constexpr int kThumbRenderBudgetMs = 5000;

// PDFium global state (font cache, page-parser internals) is NOT per-document.
// Concurrent calls from thumbnail threads and the main renderer collide and
// cause STATUS_BREAKPOINT crashes — especially on complex/large-page PDFs that
// exercise more shared PDFium state. Use the same global mutex as the main
// renderer so all PDFium calls are fully serialized.
extern QMutex s_pdfiumMutex;

// ── ThumbnailWorker ───────────────────────────────────────────────────────────

ThumbnailWorker::ThumbnailWorker(FPDF_DOCUMENT doc, int slot, TileCacheFile* cache, quint64 epoch,
                                 std::atomic<bool>* renderPaused, ThumbnailRenderPool* pool, QObject* parent)
    : QThread(parent), m_doc(doc), m_slot(slot), m_cache(cache), m_epoch(epoch),
      m_renderPaused(renderPaused), m_pool(pool) {}

void ThumbnailWorker::enqueue(int pageIndex, int priority) {
    // 0928 LƯỢT 23 (DO): TORREADER_NO_THUMB=1 ⇒ hàng đợi thumbnail không bao giờ
    // nhận việc. Chỉ để đo A/B: lap 2 chậm có phải vì tranh khoá với thumbnail
    // hay vì bản thân mỗi object PDFium đắt hơn. Không phải tính năng.
    static const bool kNoThumb = qEnvironmentVariableIsSet("TORREADER_NO_THUMB");
    if (kNoThumb) return;
    QMutexLocker lock(&m_mutex);
    if (m_stop) return;
    // Trang đang được yêu cầu lại ⇒ bỏ khỏi danh sách chờ, kể cả khi nó đang
    // NỀN bị cắt. Ưu tiên trong hàng KHÔNG lưu: `takeNext` tính lại theo vùng
    // mỗi lần bốc, nên ở đây chỉ cần tránh nhân bản cùng một trang.
    for (int i = m_parked.size() - 1; i >= 0; --i)
        if (m_parked[i].pageIndex == pageIndex) m_parked.remove(i);
    for (const auto& r : m_queue)
        if (r.pageIndex == pageIndex) return;
    m_queue.push_back(ThumbRequest{pageIndex, priority, 0, false});
    // 0928 LƯỢT 15: `xepHang` = số phần tử trong hàng sau khi xếp. Một hàng
    // không có "thứ tự chỗ" để in ra; độ dài hàng mới là thứ đo được được và là
    // thứ CEO cần (hàng phình 350 phần tử chính là lý do ô đang hiện đứng chờ).
    qDebug() << "[thumbq] uu=" << priority << "trang=" << pageIndex
             << "xepHang=" << m_queue.size();
    m_cond.wakeOne();
}

// 🔴 0928 LƯỢT 16 — ĐÓNG BĂNG TAB NỀN. Hàng đợi KHÔNG bị xoá: bỏ băng là bốc tiếp
// đúng chỗ cũ, nên "quay lại tab cũ thì thumbnail tiếp tục" không cần làm lại.
bool ThumbnailWorker::setFrozen(bool f) {
    QMutexLocker lock(&m_mutex);
    if (m_frozen == f) return false;
    m_frozen = f;
    if (!f) m_cond.wakeAll();   // mở băng: đánh thức worker đang ngủ ở đầu vòng
    return true;
}

void ThumbnailWorker::setBand(int first, int last) {
    QMutexLocker lock(&m_mutex);
    if (m_bandLo.load(std::memory_order_relaxed) == first
        && m_bandHi.load(std::memory_order_relaxed) == last)
        return;   // cuộn trong cùng vùng: không đụng hàng (cuộn nhanh sinh rất nhiều lần gọi)
    m_bandLo.store(first, std::memory_order_relaxed);
    m_bandHi.store(last, std::memory_order_relaxed);
    // 🔴 0928 LƯỢT 15c — KHÔNG còn `make_heap` ở đây. Hàng là `QVector` và thứ
    // tự xếp ở `takeNext`: chỉ cần đổi 2 số này là lần bốc kế tiếp tự ưu tiên
    // ô đang hiện. Trước đây lượt này phải rút hết hàng ra `make_heap` lại, mà
    // chính việc rút bằng `top()/pop()` với comparator đã đổi là lý do log 15b
    // còn 3 yêu cầu `uu=0` mà worker vẫn bốc `uu=2`.
    qDebug() << "[thumbq] vung moi" << first << "-" << last << "da sap lai xepHang=" << m_queue.size();
}

// ── takeNext ──────────────────────────────────────────────────────────────────
// 🔴 0928 LƯỢT 15c — BỐC VIỆC. Đây là chỗ duy nhất quyết định trang nào được
// render, nên thứ tự ở đây là luật:
//   1) tầng 0 (đang hiện) thắng tầng 1 (lân cận) thắng tầng 2 (nền) — không
//      bao giờ lấy nền khi còn 0/1. Đây là luật chặn vòng quay 15b: worker bốc
//      trang 74 nền trong khi trang 152–154 `uu=0` đứng ngay trong hàng.
//   2) trong tầng 0/1: số trang nhỏ trước ⇒ đúng thứ tự trên→dưới ở khung.
//   3) trong tầng nền: trang GẦN vùng nhất, không đi tuần tự từ đầu file.
//   4) hàng cạn mới gỡ `m_parked` (việc nền bị cắt). Không gỡ sớm = không quay
//      vòng; gỡ muộn cũng không mất gì vì lượt cuộn tới ô đó gọi `enqueue` và
//      `enqueue` tự gỡ trang đó khỏi danh sách chờ.
bool ThumbnailWorker::takeNext(ThumbRequest* out) {
    if (m_queue.isEmpty() && !m_parked.isEmpty()) {
        // Ưu tiên đã bị hạ vì lúc cắt, tính lại theo vùng lúc gỡ (trang đã
        // cuộn vào khung thì lên tầng 0 ngay); cờ `provisional` giữ nguyên để
        // lần này render trọn, không cắt nữa.
        for (const auto& r : m_parked)
            m_queue.push_back(ThumbRequest{r.pageIndex, tierFor(r.pageIndex),
                                           r.yieldTries, r.provisional});
        m_parked.clear();
    }
    int best = -1, bestTier = 3, bestTie = 0;
    for (int i = 0; i < m_queue.size(); ++i) {
        const int p = m_queue[i].pageIndex;
        const int t = tierFor(p);
        const int tie = (t >= 2) ? bandDist(p) : p;
        if (best < 0 || t < bestTier || (t == bestTier && tie < bestTie)) {
            best = i; bestTier = t; bestTie = tie;
        }
    }
    if (best < 0) return false;
    *out = m_queue.takeAt(best);
    out->priority = bestTier;                    // ưu tiên TÍNH LẠI lúc bốc
    return true;
}

void ThumbnailWorker::stop() {
    QMutexLocker lock(&m_mutex);
    m_stop = true;
    // 0903: don hang doi de close()/wait() KHONG phai render het so thumbnail pending
    // (mot lan dong tab co the con >100 request trong hang doi → drain = treo UI).
    m_queue.clear();
    m_parked.clear();
    m_cond.wakeAll();
}

void ThumbnailWorker::run() {
    while (true) {
        ThumbRequest req{-1, 2};
        {
            QMutexLocker lock(&m_mutex);
            // 🔴 0928 LƯỢT 16 — `m_frozen` (tab nền / đang đóng tab): KHÔNG bốc việc.
            // Ngủ trên cond, `setFrozen(false)` đánh thức ⇒ hàng giữ nguyên.
            while (!m_stop && (m_frozen || !takeNext(&req)))
                m_cond.wait(&m_mutex);
            // 0903: stop = HUY toan bo, khong drain hang doi → wait() trong close()
            // co buoc, khong treo UI thread khi dong tab.
            if (m_stop) break;
        }

        if (!m_doc || req.pageIndex < 0) continue;

        // 🔴 0928 LƯỢT 15c — LOG BẮT BUỘC mỗi lần worker bốc việc. LƯỢT 15/15b
        // không có dòng này nên phải đoán ngược từ `[thumb song song] page=N`
        // mới thấy worker bốc 63→147 trong khi 152–154 uu 0 đứng đó. `cachVung`
        // cho biết chính trang đó đã xa vùng bao nhiêu lúc bốc.
        // 🔴 0928 LƯỢT 16 — thêm `doc=`: log r8 cho thấy trang 84–87 của MEP bốc
        // liên tục trong lúc tab3 đang hiện mà không in ra tài liệu nào đang được
        // so với vùng. Từ giờ mỗi dòng bốc nói rõ doc nào.
        qDebug() << "[thumbq] lay trang=" << req.pageIndex << "uu=" << req.priority
                 << "cachVung=" << bandDist(req.pageIndex) << "doc=" << (void*)m_doc;

        // ── Check disk cache first (no pdfium mutex needed) ──
        if (m_cache && m_cache->isOpen()) {
            QImage cached = m_cache->readPage(req.pageIndex, CacheZoom::Thumb);
            if (!cached.isNull()) {
                qDebug() << "[perf] thumb cache hit page=" << req.pageIndex;
                qDebug() << "[perf] thumb WORKER emit page=" << req.pageIndex << "worker=" << (void*)this << "thread=" << QThread::currentThreadId();
                emit thumbnailReady(req.pageIndex, cached, m_epoch, /*draft=*/false);
                continue;
            }
        }

        // 🔴 LƯỢT 30 (VIỆC 2, B) + LƯỢT 31 (A1): trang QUÁI VẬT (≥1M object) — ĐỪNG
        // parse lại trên handle pool RIÊNG chỉ để lấy thumbnail: nó thêm ~1,5 GB object
        // cache và hàng giây CPU, làm CHẬM THÊM trang 3. Nếu đã có ẢNH CHÍNH
        // (bestCachedForPage — do lượt vẽ trang tạo ra, KỂ CẢ ảnh Continuous vì
        // Continuous cũng nạp vào m_cache ở PdfRenderer.cpp:2114) ⇒ THU NHỎ nó, không
        // đụng PDFium. CHƯA có ảnh chính ⇒ NHƯỜNG, nhưng KHÔNG được bỏ vĩnh viễn:
        // sau tran THOI GIAN (L31b: 10 s tu lan dau gap) thi VE THAT 1 LAN o co
        // thumbnail (luoi chong o trang — chinh la canh comment L30 hen ma code chua lam).
        if (PdfRenderer* rend = (m_pool ? m_pool->pdfRenderer() : nullptr)) {
            const int nObj = rend->pageObjectCount(req.pageIndex);
            if (nObj >= 1000000) {
                QImage full = rend->bestCachedForPage(req.pageIndex);
                if (!full.isNull()) {
                    const int tw = int(PdfRenderer::kThumbMaxPx);
                    QImage th = full.width() > tw
                              ? full.scaledToWidth(tw, Qt::SmoothTransformation) : full;
                    if (m_cache && m_cache->isOpen())
                        m_cache->writePage(req.pageIndex, CacheZoom::Thumb, th);
                    qDebug().noquote() << "[perf] thumb TAI-SU-DUNG anh chinh page="
                                       << req.pageIndex << "objs=" << nObj;
                    { QMutexLocker lock(&m_mutex); m_heavySkipSince.remove(req.pageIndex); }
                    emit thumbnailReady(req.pageIndex, th, m_epoch, /*draft=*/false);
                    continue;
                }
                // 🔴 LƯỢT 31b (reviewer mục 5): tran theo THOI GIAN — nhường tối đa
                // kHeavyThumbYieldMs (10 s) kể từ LẦN ĐẦU gặp trang nặng chưa có ảnh
                // chính, rồi vẽ thật 1 lần (xóa mốc). Đếm lượt cũ (3) bị ba vòng
                // requestVisibleThumbnails liên tiếp lúc mở tab đốt hết trước khi ảnh
                // chính về ⇒ pool parse lại trang 2,18M object — đúng điều L30 muốn tránh.
                bool giveUpWaiting = false;
                {
                    QMutexLocker lock(&m_mutex);
                    const qint64 now = QDateTime::currentMSecsSinceEpoch();
                    if (!m_heavySkipSince.contains(req.pageIndex))
                        m_heavySkipSince.insert(req.pageIndex, now);   // lan dau gap
                    giveUpWaiting = (now - m_heavySkipSince.value(req.pageIndex)) >= kHeavyThumbYieldMs;
                    if (giveUpWaiting) m_heavySkipSince.remove(req.pageIndex);  // ve 1 lan
                }
                if (!giveUpWaiting) {
                    qDebug().noquote() << "[perf] thumb NHUNG page=" << req.pageIndex
                                       << "objs=" << nObj << " (cho anh chinh, khong tu ve)";
                    continue;
                }
                qDebug().noquote() << "[perf] thumb LUOI-ve-1-lan page=" << req.pageIndex
                                   << "objs=" << nObj << "(het anh chinh, chong o trang)";
                // rơi xuống nhánh render bình thường bên dưới
            }
        }

        // 🔴 0928 LƯỢT 13 — NHƯỜNG ĐƯỜNG HIỂN THỊ. Trang đang xem đang chờ khoá
        // PDFium thì thumbnail ĐỪNG giành. Log khoachung: [lockwait] ms= 2346 at
        // PdfRenderer.cpp:480 (đường raster của trang đang xem) trong khi pool nền
        // vẫn lấy khoá ở nhánh pool-handle — nhánh đó TRƯỚC ĐÂY không có chốt
        // nhường nào (chỉ nhánh PageCache mới có `yieldTries`), và đó đúng là chỗ
        // sinh [lockhold] ms= 820 tại dòng :152.
        // 🔴 0928 LƯỢT 16 — GIỮ VIỆC KHI NHƯỜNG, KHÔNG xếp lại hàng để bốc lại.
        // Đo log r8: từ 53.390 tới 53.934 worker in `[thumbq] lay trang= 87` 16
        // lần trong 0,5 s — mỗi lần nhường là `requeueLocked` + `wakeOne` + bốc
        // lại đúng trang đó. Không render thêm một trang nào, chỉ đốt CPU + log.
        // Nay giữ nguyên `req` trong vòng nhừng 20 ms: hết khẩn thì vẽ tiếp chính
        // nó; bị đóng băng giữa chừng thì mới trả về hàng.
        // 🔴 LƯỢT 16b — vòng nhường KHÔNG được chặn tầng 0 (bản 16 giữ `req` tới
        // ~4 s mà không tính lại tier ⇒ ô vừa cuộn vào khung xếp hàng sau trang
        // nền đang bị nhường). Mỗi vòng: tierFor(req) lên 0 ⇒ vẽ luôn; hàng có
        // việc tầng 0 khác mà req không phải tầng 0 ⇒ requeueLocked(req) MỘT lần
        // rồi bốc lại — takeNext bốc theo tier nên trang bị trả KHÔNG được bốc
        // lại khi còn việc 0/1 khác, không quay về vòng lặp 16 lần/0,5 s của r8.
        if ((pdfiumUrgentPending() || thumbUrgentWindow()) && req.priority != 0) {
            bool stopped = false, requeued = false;
            for (;;) {
                if (req.yieldTries == 0 || req.yieldTries % 20 == 0)
                    qDebug() << "[khoa] thumb NHUONG page=" << req.pageIndex
                             << "tries=" << req.yieldTries << "vi trang dang xem can khoa";
                QThread::msleep(20);
                QMutexLocker lock(&m_mutex);
                if (m_stop) { stopped = true; break; }
                if (m_frozen) { requeueLocked(req); requeued = true; break; }
                req.yieldTries++;
                req.priority = tierFor(req.pageIndex);          // tier TÍNH LẠI mỗi vòng
                if (req.priority == 0) break;                   // trang vào vùng hiện — vẽ luôn
                bool tier0ChoWaiting = false;
                for (const auto& q : m_queue)
                    if (tierFor(q.pageIndex) == 0) { tier0ChoWaiting = true; break; }
                if (tier0ChoWaiting) { requeueLocked(req); requeued = true; break; }
                if (!pdfiumUrgentPending() && !thumbUrgentWindow()) break;  // hết khẩn — vẽ tiếp
                if (req.yieldTries > kThumbUrgentMaxYields) {   // chống đói: cứ làm
                    req.priority = 0;
                    break;
                }
            }
            if (stopped) break;
            if (requeued) continue;
        }

        // Dang tam dung: KHONG dung PDFium. Tra yeu cau ve hang doi roi ngu ngan,
        // de trang nguoi dung dang xem duoc uu tien dung xong truoc.
        if (m_renderPaused && m_renderPaused->load()) {
            {
                QMutexLocker lock(&m_mutex);
                if (m_stop) break; // dung lai: khong day lai hang doi mai mai
                requeueLocked(req);
            }
            QThread::msleep(50);
            continue;
        }

        // 🔴 0928 LƯỢT 18 (VIỆC 2): KHÔNG vẽ trang mà view chính đang chờ ảnh.
        // Đo log r17: thumbnail trang 3 giữ khoá 1335 ms (FPDF_LoadPage pool handle)
        // đúng lúc render trang 3 cần khoá — nó đứng chai từ lát 3797 sang 5233 ms.
        // Hai lượt cùng MỘT trang thì lượt nền (thumbnail) phải nhường cho tới khi
        // ảnh chính về (ContinuousView xoa g_pdfiumUrgentPage luc ACCEPT).
        // Chống đói: 50 x 100 ms = 5 s — qua đó thi vẫn vẽ (view chết non / khác
        // tài liệu), không được để ô trống vĩnh viễn.
        // 🔴 0928 LƯỢT 19 (CEO 18.7-ii): thumbnail CŨNG nhường khi view chính của
        // tab hiện hành CHƯA có ảnh primary — KỂ CẢ tầng 0 (bản L18 chỉ nhường
        // đúng trang trùng primary; L16b cố ý không chặn tier-0 nên tier-0 trang
        // 1/2 vẫn ping-pong khoá với 24 lát của trang 3, ~4,3 s). Trần 5 s tính
        // TỪ LÚC primary bắt đầu chờ (g_pdfiumUrgentPageDeadline, không phải mỗi
        // request) — qua trần vẽ bình thường, chống đói. Có ảnh primary (GUI xóa
        // g_pdfiumUrgentPage lúc ACCEPT) ⇒ vòng này thoát ngay, thumbnail chạy lại
        // tức thì. Không đụng tier/hàng đợi L15c/16/16b — chỉ thêm chốt nhường.
        if (g_pdfiumUrgentPage().load(std::memory_order_acquire) >= 0
            && QDateTime::currentMSecsSinceEpoch()
               <= g_pdfiumUrgentPageDeadline().load(std::memory_order_acquire)) {
            bool stopped = false, requeued = false;
            int lat = 0;
            for (;;) {
                {
                    QMutexLocker lock(&m_mutex);
                    if (m_stop)   { stopped = true; break; }
                    if (m_frozen) { requeueLocked(req); requeued = true; break; }
                }
                if (g_pdfiumUrgentPage().load(std::memory_order_acquire) < 0) break;   // co anh / doi primary -> ve ngay
                if (QDateTime::currentMSecsSinceEpoch()
                    > g_pdfiumUrgentPageDeadline().load(std::memory_order_acquire)) break;  // qua tran 5s -> ve binh thuong
                if (lat == 0 || lat % 10 == 0)   // 1 dong/giay, chong phinh log
                    qDebug() << "[khoa] thumb NHUONG-trang-chinh page=" << req.pageIndex
                             << "uu=" << req.priority << "vi primary chua co anh lan=" << lat;
                ++lat;
                QThread::msleep(100);   // lat 100 ms theo CEO
            }
            if (stopped) break;
            if (requeued) continue;
        }

        // ── Progressive render via PDFium (releases mutex between slices) ──

        // 0928 LƯỢT 14: token sổ việc nền. Nhánh PageCache phía dưới mượn trang
        // của doc CHÍNH qua PageCache. `ThumbnailRenderPool::close()` đã có
        // `w->stop(); w->wait()` (chặn thật), nhưng token cho sổ biết còn bao
        // nhiêu việc và giữ doc sống nếu wait() bị bỏ qua. Đặt SAU hai chốt
        // nhường/pause vì chúng KHÔNG đụng PDFium.
        // 🔴 LƯỢT 22 — MIỄN CHUYỂN "đăng ký lúc spawn": đây là VÒNG worker thread
        // nội bộ (không phải hàng đợi QtConcurrent). Job xếp hàng trong hàng đợi
        // nội bộ chưa chạm PDFium/`m_doc`; shutdownTab gọi stopThumbPool() =
        // close() = stop()+wait() TRƯỚC khi UI đụng PDFium ⇒ không có cửa sổ
        // "xếp hàng mà beginClose không thấy".
        trdoc::Task task(m_doc, "ThumbnailWorker");

        QImage image;
        QElapsedTimer timer;
        timer.start();

        FPDF_PAGE   fpdfPage = nullptr;
        FPDF_BITMAP bmp      = nullptr;
        int renderStatus     = FPDF_RENDER_READY;
        FPDF_DOCUMENT poolHandle = nullptr;  // VIỆC: pool handle nếu có
        bool usingPoolHandle = false;  // VIỆC: ghi dấu để biết cách cleanup

        // 🔴 RAII 2026-08-31 — DO DUOC RO RI: 79 luot muon / 78 luot tra.
        // Muon o MOT cho (duoi day) nhung tra tay o BA cho cach nhau 142 dong, va giua chung
        // co 4 lenh return/continue. Chi can MOT duong thoat quen tra la handle mat vinh vien;
        // pool chi co 3 slot nen dung lau la can sach => moi thu lui ve o khoa chung
        // => app CANG DUNG CANG CHAM. Guard nay bao dam tra du moi duong thoat.
        struct PoolHandleGuard {
            ThumbnailRenderPool* pool = nullptr;
            FPDF_DOCUMENT h = nullptr;
            void release() {   // goi khi da tra tay roi, de guard khoi tra lan hai
                pool = nullptr; h = nullptr;
            }
            ~PoolHandleGuard() {
                if (pool && h && pool->pdfRenderer())
                    pool->pdfRenderer()->returnPoolHandleForThumbnail(h);
            }
        } _phGuard;

        // Step 0: Thử lấy pool handle từ renderer nếu có
        if (m_pool && m_pool->pdfRenderer()) {
            class PdfRenderer* renderer = m_pool->pdfRenderer();
            poolHandle = renderer->borrowPoolHandleForThumbnail();
            if (poolHandle) {
                usingPoolHandle = true;
                _phGuard.pool = m_pool;
                _phGuard.h    = poolHandle;
                qDebug() << "[thumb song song] page=" << req.pageIndex << "dung pool handle";
            }
        }

        // Step 1: Start (load page từ pool handle hoặc PageCache)
        {
            if (usingPoolHandle) {
                // 🔴 LƯỢT 12: khoá PDFium TRÊN handle pool như mọi handle khác (đo
                // TSan/ASan: 229 tranh chấp + use-after-free ColorSpace toàn cục).
                MaybePdfiumLock lkPoolLoad(__FILE__, __LINE__, trPoolLockOn(), req.pageIndex);
                fpdfPage = FPDF_LoadPage(poolHandle, req.pageIndex);
                // 🔴 THỨ TỰ KHOÁ: trả handle (nhận m_handlePoolMutex) NGOÀI khoá
                // PDFium — `initHandlePool` giữ m_handlePoolMutex rồi mới xin
                // s_pdfiumMutex, gọi lồn là AB-BA ⇒ treo app.
                lkPoolLoad.unlock();
                if (!fpdfPage) {
                    qDebug() << "[perf] thumb pool load page failed page=" << req.pageIndex;
                    if (m_pool && m_pool->pdfRenderer())
                        m_pool->pdfRenderer()->returnPoolHandleForThumbnail(poolHandle); _phGuard.release();
                    continue;
                }
                MaybePdfiumLock lkPoolOpen(__FILE__, __LINE__, trPoolLockOn(), req.pageIndex);
                PdfCloseTrace::openPage(poolHandle, fpdfPage, req.pageIndex,
                                        "ThumbnailWorker::run[pool]");
            } else {
                // ĐƯỜNG CŨ: dùng mutex + PageCache
                // Yield to main renderer for ALL priorities: thumbnail delayed a few hundred ms
                // is invisible to user, but main page stuck 15s is very visible.
                // Anti-starvation: after kThumbYieldMaxTries, take lock by force so thumbnails still appear.
                constexpr int kThumbYieldMaxTries = 25; // ~25 x (5ms try + 20ms sleep) ~ 600ms
                if (req.yieldTries < kThumbYieldMaxTries) {
                    if (!s_pdfiumMutex.tryLock(5)) {
                        QMutexLocker lock(&m_mutex);
                        req.yieldTries++;
                        if (req.yieldTries % 10 == 0)
                            qDebug() << "[perf] thumb YIELD page=" << req.pageIndex
                                     << "tries=" << req.yieldTries;
                        requeueLocked(req);
                        QThread::msleep(20);
                        continue;
                    }
                    s_pdfiumMutex.unlock();
                } else {
                    qDebug() << "[perf] thumb FORCE lock page=" << req.pageIndex
                             << "(het luot nhuong)";
                }

                TimedPdfiumLock thumbLock(__FILE__, __LINE__, req.pageIndex);

                // R1 (SPEC_PERF_HEAVYPAGE): di qua PageCache — NGUON SU THAT DUY NHAT.
                // Da giu s_pdfiumMutex (thumbLock) ngay truoc do nen goi acquire() hop le.
                fpdfPage = PageCache::acquire(m_doc, req.pageIndex);
                if (!fpdfPage) continue;
            }

            // 🔴 LƯỢT 12: từ đây tới hết lát Start là PDFium trên handle ĐÃ MỠ —
            // cả HAI nhánh (pool lẫn PageCache) giờ ĐỀU khoá (trước đây nhánh
            // PageCache hết scope `thumbLock` ở trên, còn nhánh pool thì không
            // khoá). Lát này NHẢ khoá ở khoảng trống giữa các lát Continue.
            MaybePdfiumLock lkStart(__FILE__, __LINE__, trPoolLockOn(), req.pageIndex);

            // ── Try embedded thumbnail first ──
            FPDF_BITMAP tb = FPDFPage_GetThumbnailAsBitmap(fpdfPage);
            if (tb) {
                int tw = FPDFBitmap_GetWidth(tb);
                int th = FPDFBitmap_GetHeight(tb);
                int stride = FPDFBitmap_GetStride(tb);
                const uchar* buf = (const uchar*)FPDFBitmap_GetBuffer(tb);
                QImage embImg = QImage(buf, tw, th, stride, QImage::Format_ARGB32).copy();
                FPDFBitmap_Destroy(tb);

                // Đóng trang PDFium + trả handle TRƯỚC khi nhả khoá; ghi đĩa .torcache
                // và emit tín hiệu (việc nặng, không phải PDFium) thì LÀM NGOÀI khoá
                // — VIỆC 2: không giữ khoá PDFium khi chờ/ghi file.
                if (usingPoolHandle) {
                    PdfCloseTrace::closePage(poolHandle, fpdfPage, "ThumbnailWorker::run[pool]");
                }
                lkStart.unlock();
                if (usingPoolHandle) {
                    // 🔴 THỨ TỰ KHOÁ: ngoài khoá PDFium rồi mới lấy m_handlePoolMutex
                    // (`initHandlePool` đi theo chiều ngược lại ⇒ AB-BA thì treo app).
                    if (m_pool && m_pool->pdfRenderer())
                        m_pool->pdfRenderer()->returnPoolHandleForThumbnail(poolHandle);
                    _phGuard.release();
                } else {
                    PageCache::release(m_doc, req.pageIndex, fpdfPage);
                }

                qDebug() << "[perf] thumb done page=" << req.pageIndex
                         << "engine=embedded ms=" << timer.elapsed();
                if (m_cache && m_cache->isOpen())
                    m_cache->writePage(req.pageIndex, CacheZoom::Thumb, embImg);
                qDebug() << "[perf] thumb WORKER emit page=" << req.pageIndex << "worker=" << (void*)this << "thread=" << QThread::currentThreadId();
                emit thumbnailReady(req.pageIndex, embImg, m_epoch, /*draft=*/false);
                continue;   // handle muon tu PageCache hoac pool — khong can FPDF_ClosePage trong cac truong hop khac
            }

            double w = FPDF_GetPageWidth(fpdfPage);
            double h = FPDF_GetPageHeight(fpdfPage);
            int imgW = qMax(1, (int)(w * ThumbnailRenderPool::kThumbScale));
            int imgH = qMax(1, (int)(h * ThumbnailRenderPool::kThumbScale));

            image = QImage(imgW, imgH, QImage::Format_ARGB32);
            image.fill(Qt::white);

            bmp = FPDFBitmap_CreateEx(imgW, imgH, FPDFBitmap_BGRA,
                                      image.bits(), image.bytesPerLine());

            ThumbPauseCtx pctx;
            pctx.timer.start();
            IFSDK_PAUSE pause;
            pause.version = 1;
            pause.NeedToPauseNow = ThumbNeedToPauseNow;
            pause.user = &pctx;

            renderStatus = FPDF_RenderPageBitmap_Start(bmp, fpdfPage, 0, 0,
                                                       imgW, imgH, 0,
                                                       FPDF_RENDER_LIMITEDIMAGECACHE | FPDF_RENDER_NO_SMOOTHTEXT | FPDF_RENDER_NO_SMOOTHIMAGE | FPDF_RENDER_NO_SMOOTHPATH,
                                                       &pause);
        }
        PdfCloseTrace::renderOpen(poolHandle, fpdfPage, "ThumbnailWorker::run[pool]");

        // Step 2: Continue loop
        // 🔴 0928 LƯỢT 15 — HAI ĐƯỜNG THOÁT SỚM, đều ở ranh giới lát nên không
        // phải huỷ PDFium giữa chừng:
        //   • quá trần thời gian ⇒ phát bản nháp, xếp vào danh sách chờ;
        //   • trang đã XA khỏi vùng đang xem (người dùng cuộn mất) ⇒ bỏ luôn
        //     việc nền này, xếp vào danh sách chờ.
        // Cả hai dùng chung cờ `provisional`: đã cắt một lần thì lần sau render
        // trọn ⇒ không bao giờ lặp.
        // LƯỢT 15c: việc uu 0/1 không bao giờ xa quá `kAdjacentDist` (đó là định
        // nghĩa của tầng 1) ⇒ nhánh "bỏ vì xa" chỉ bắt được việc NỀN, đúng ý.
        bool outOfBudget = false, abandoned = false, midYield = false;
        while (renderStatus == FPDF_RENDER_TOBECONTINUED) {
            if (timer.elapsed() >= kThumbRenderBudgetMs) { outOfBudget = true; break; }
            if (!req.provisional && bandDist(req.pageIndex) > kAdjacentDist) {
                abandoned = true;
                break;
            }
            {
                QMutexLocker lock(&m_mutex);
                if (m_stop) break;
            }
            {
                if (usingPoolHandle) {
                    // Lát Continue: khoá quanh đúng lệnh PDFium rồi nhả — y hệt
                    // nhánh PageCache ngay dưới, giữ nhịp "UI không đứng".
                    MaybePdfiumLock lkPoolCont(__FILE__, __LINE__, trPoolLockOn(), req.pageIndex);
                    ThumbPauseCtx pctx;
                    pctx.timer.start();
                    IFSDK_PAUSE pause;
                    pause.version = 1;
                    pause.NeedToPauseNow = ThumbNeedToPauseNow;
                    pause.user = &pctx;
                    renderStatus = FPDF_RenderPage_Continue(fpdfPage, &pause);
                } else {
                    // Đường cũ: cần khoá
                    TimedPdfiumLock thumbLock(__FILE__, __LINE__, req.pageIndex);

                    ThumbPauseCtx pctx;
                    pctx.timer.start();
                    IFSDK_PAUSE pause;
                    pause.version = 1;
                    pause.NeedToPauseNow = ThumbNeedToPauseNow;
                    pause.user = &pctx;

                    renderStatus = FPDF_RenderPage_Continue(fpdfPage, &pause);
                }
            }
            // 🔴 0928 LƯỢT 24 (23.5) — NHƯỜNG GIỮA LÁT. Chốt L18/L19 chỉ kiểm lúc
            // BỐC VIỆC: một ô thumbnail đã chạy dở (đo r23 run1: trang 3 giữ khoá
            // 3 130 ms / 14 lát trong khi primary trang 6 chờ ảnh ⇒ FAIL 5 749 ms)
            // không bao giờ bị hỏi lại. Nay hỏi SAU MỖI LÁT: primary đang chờ ảnh
            // (g_pdfiumUrgentPage >= 0, ContinuousView xoá lúc ACCEPT — tính cả
            // trang trùng với ô này, vì hai lượt cùng trang chỉ tổ tranh khoá)
            // mà chưa qua trần chống đói ⇒ DỪNG ở ranh giới lát (trạng thái
            // PDFium sạch — cùng nguyên tắc L15), Step 3 đóng gọn, phát bản nháp
            // và xếp vào danh sách chờ; hàng về và qua trần thì vẽ lại trọn.
            // Trần = g_pdfiumUrgentPageDeadline (5 s, hoặc 30 s cho trang ≥1M obj
            // do armHeavyThumbYield nâng) — hết trần là vẽ tiếp, không đói.
            // 0928 LƯỢT 25 (reviewer L23+L24 mục 2): `req.provisional` = ĐÃ bị cắt
            // một lần — luật L15 "mỗi trang tối đa một lần bị cắt". Nhường giữa lát
            // lần hai sẽ cắt-vẽ lại mãi khi primary chờ dài hạn ⇒ bỏ qua nếu provisional.
            if (renderStatus == FPDF_RENDER_TOBECONTINUED
                && !req.provisional
                && g_pdfiumUrgentPage().load(std::memory_order_acquire) >= 0
                && QDateTime::currentMSecsSinceEpoch()
                   <= g_pdfiumUrgentPageDeadline().load(std::memory_order_acquire)) {
                qDebug() << "[khoa] thumb NHUONG-GIUA-LAT page=" << req.pageIndex
                         << "uu=" << req.priority << "ms=" << timer.elapsed()
                         << "vi primary chua co anh";
                midYield = true;
                break;
            }
        }

        // Step 3: Close
        bool usingPoolForClose = usingPoolHandle;
        FPDF_DOCUMENT closeHandle = poolHandle;
        {
            // 🔴 LƯỢT 12: FPDF_RenderPage_Close + FPDFBitmap_Destroy + FPDF_ClosePage
            // nay GIỮ KHOÁ (trước đây không khoá ở CẢ HAI nhánh — đó là 3 trong 26
            // chỗ gọi PDFium ngoài luồng UI không khoá; xem REPORT_CRASH_EYACHO_0927.md).
            MaybePdfiumLock lkClose(__FILE__, __LINE__, trPoolLockOn(), req.pageIndex);
            if (renderStatus == FPDF_RENDER_TOBECONTINUED || renderStatus == FPDF_RENDER_DONE) {
                PdfCloseTrace::renderClose(poolHandle, fpdfPage, "ThumbnailWorker::run[pool]");
                FPDF_RenderPage_Close(fpdfPage);
            }
            if (bmp) { FPDFBitmap_Destroy(bmp); bmp = nullptr; }

            if (usingPoolHandle) {
                // 🔴 SUA 2026-08-31 — RO RI TRANG PDFium (goc cua ~5 GB RAM).
                // Nhanh nay tra HANDLE ve pool nhung QUEN dong TRANG da mo tren handle do.
                // Handle duoc muon lai, mo them trang nua, lai khong dong => trang tich tu
                // tren 12 handle moi tab. DO DUOC: moi trang A0 dang mo ton ~37 MB bo nho
                // noi bo PDFium (giu 40 trang = 1.639 MB, dong het = 191 MB).
                // Voi ~86 thumbnail di duong nay thi ~3,2 GB — dung co phan RAM khong giai
                // thich duoc. Nhanh embedded o tren (dong ~201) VAN LUON dong dung.
                if (fpdfPage) {
                    PdfCloseTrace::closePage(poolHandle, fpdfPage, "ThumbnailWorker::run[pool]");
                    fpdfPage = nullptr;
                }
            }
        }
        // 🔴 LƯỢT 12 — THỨ TỰ KHOÁ: trả handle (nhận `m_handlePoolMutex`) phải NGOÀI
        // khoá PDFium, vì `PdfRenderer::initHandlePool` (:1170) giữ `m_handlePoolMutex`
        // RỒI mới xin `s_pdfiumMutex`. Gọi lồn nhau là AB-BA ⇒ treo app.
        if (usingPoolForClose) {
            if (m_pool && m_pool->pdfRenderer())
                m_pool->pdfRenderer()->returnPoolHandleForThumbnail(closeHandle);
            _phGuard.release();
        } else {
            PageCache::release(m_doc, req.pageIndex, fpdfPage);
        }

        // 🔴 0928 LƯỢT 15 — phát ảnh + xử lý việc bị cắt.
        // `renderStatus` vẫn là TOBECONTINUED khi bị cắt ⇒ nhánh đóng ở trên đã
        // gọi `FPDF_RenderPage_Close` xong rồi, trạng thái PDFium sạch.
        const bool complete = (renderStatus == FPDF_RENDER_DONE);
        if (abandoned) {
            qDebug() << "[thumbq] BO nen xa page=" << req.pageIndex
                     << "cachVung=" << bandDist(req.pageIndex) << "ms=" << timer.elapsed();
        } else if (!image.isNull() && (complete || outOfBudget || midYield)) {
            if (complete) {
                qDebug() << "[perf] thumb done page=" << req.pageIndex << "ms=" << timer.elapsed();
                if (m_cache && m_cache->isOpen())
                    m_cache->writePage(req.pageIndex, CacheZoom::Thumb, image);
            } else {
                // ⛔ KHÔNG ghi đĩa .torcache: đây là ảnh DỞ, ghi vào sẽ đóng băng
                // ô trắng cho mọi lần mở sau.
                qDebug() << "[perf] thumb NHAP page=" << req.pageIndex
                         << "ms=" << timer.elapsed()
                         << (midYield ? "(nhuong giua lat, chua ve xong)"
                                      : "(het tran, chua ve xong)");
            }
            qDebug() << "[perf] thumb WORKER emit page=" << req.pageIndex << "worker=" << (void*)this << "thread=" << QThread::currentThreadId();
            emit thumbnailReady(req.pageIndex, image, m_epoch, /*draft=*/!complete);
        } else if (renderStatus == FPDF_RENDER_FAILED) {
            qDebug() << "[perf] thumb FAILED page=" << req.pageIndex;
        }

        // Việc bị CẮT (bỏ vì xa) hoặc mới phát bản nháp ĐI VÀO DANH SÁCH CHỜ,
        // KHÔNG xếp lại ngay. Lượt 15 xếp lại ngay và đó chính là vòng quay
        // đo được trong log 15b: 63→147, mỗi trang bị bỏ dở sau ~100 ms rồi bị
        // bốc lại, không trang nào xong, 12 s không ra một ô nào ở vùng 17x–19x.
        // Chỉ `takeNext` gỡ lại danh sách này, và chỉ khi hàng không còn uu 0/1.
        // `provisional` = true ⇒ lần sau render cho trọn, không cắt nữa (chống
        // lặp: mỗi trang tối đa một lần bị cắt).
        if (abandoned || outOfBudget || midYield) {
            QMutexLocker lock(&m_mutex);
            bool parked = false;
            for (const auto& r : m_parked)
                if (r.pageIndex == req.pageIndex) { parked = true; break; }
            if (!m_stop && !parked) {
                m_parked.push_back(ThumbRequest{req.pageIndex, 2, 0, /*provisional=*/true});
                qDebug() << "[thumbq] CHO page=" << req.pageIndex
                         << "cachVung=" << bandDist(req.pageIndex)
                         << "choHang=" << m_parked.size();
            }
            // Đánh thức: nếu hàng rỗng thì `takeNext` ở vòng sau sẽ gỡ danh
            // sách này ra và bốc — không thì worker ngủ vĩnh với việc chờ.
            m_cond.wakeOne();
        }
    }
    // R1: doc la MUON tu PdfDocument (shared handle) — KHONG forgetDocument, KHONG
    // FPDF_CloseDocument o day. Tai lieu chu tu dong forgetDocument + close khi dong.
}

// ── ThumbnailRenderPool ───────────────────────────────────────────────────────

ThumbnailRenderPool::ThumbnailRenderPool(QObject* parent) : QObject(parent) {}

void ThumbnailRenderPool::insertThumbnail(int pageIndex, const QImage& img) {
    if (pageIndex < 0 || img.isNull()) return;
    if (m_cache && m_cache->isOpen()) m_cache->writePage(pageIndex, CacheZoom::Thumb, img);
    qDebug().noquote() << "[perf] thumb done page=" << pageIndex
                       << "engine=vectorGPU ms=0";
    emit thumbnailReady(pageIndex, img, m_epoch, /*draft=*/false);
}

void ThumbnailRenderPool::setRenderPaused(bool paused) {
    m_renderPaused.store(paused);
}

bool ThumbnailRenderPool::isRenderPaused() const {
    return m_renderPaused.load();
}

// 🔴 0928 LƯỢT 16 — MainWindow gọi mỗi lần đổi tab / đóng tab. Trả true khi
// trạng thái THỰC SỰ đổi (caller in log `doi tab dong bang` đúng một lần).
bool ThumbnailRenderPool::setFrozen(bool f) {
    if (m_frozen == f) return false;
    m_frozen = f;
    for (auto* w : m_workers) w->setFrozen(f);
    return true;
}

void ThumbnailRenderPool::setPdfRenderer(PdfRenderer* renderer) {
    m_pdfRenderer = renderer;
}

ThumbnailRenderPool::~ThumbnailRenderPool() { close(); }

bool ThumbnailRenderPool::open(const QString& pdfPath, FPDF_DOCUMENT sharedDoc,
                               uint64_t preHash, uint64_t preSize, int prePageCount,
                               std::shared_ptr<TileCacheFile> tileCache) {
    // 0927 LƯỢT 10 (--no-thumbs): KHONG mo pool ⇒ KHONG khoi dong ThumbnailWorker
    // nao. `m_open` giu false ⇒ moi `requestThumbnail` (:436) va `prefetchRange`
    // (:442) thoat ngay, panel hien "(trang trang)" binh thuong (ThumbnailPanel.cpp
    // :659 da xu ly dung truong hop "pool FAILED to open").
    if (trNoThumbs()) { qDebug() << "[perf] thumb pool DISABLED (--no-thumbs)"; return false; }
    if (m_open) close();
    m_path = pdfPath;
    if (!sharedDoc) { qDebug() << "[perf] thumb pool FAILED to open — no shared doc"; return false; }

    // Disk cache for thumbnails (R1: dung CHUNG doc voi renderer chinh — khong parse rieng).
    // 0927 M2: dung CHUNG doi tuong cua tab, KHONG tu mo .torcache rieng. Neu da co
    // object roi thi KHONG tinh hash/size o day — hash L31 doc 64KB dau+cuoi + 8 khoi
    // 4KB giua file + stat mtime (~192 KB) tren UI thread la thua.
    if (tileCache) {
        m_cache = tileCache;              // tab so huu — pool chi muon shared_ptr
    } else {
        int pgCount = (prePageCount > 0) ? prePageCount : 0;
        if (pgCount <= 0) {
            TimedPdfiumLock thumbLock(__FILE__, __LINE__);
            pgCount = FPDF_GetPageCount(sharedDoc);
        }
        if (pgCount > 0) {
            QFileInfo fi(pdfPath);
            const uint64_t hash = preHash ? preHash : TileCacheFile::hashFile(pdfPath);
            const uint64_t size = preSize ? preSize : static_cast<uint64_t>(fi.size());
            // Khong co tab nao so huu (bench/test o main.cpp): pool tu acquire
            // trong registry ⇒ van chi la MOT doi tuong cho file nay.
            m_cache      = TileCacheRegistry::acquire(pdfPath, hash, size, pgCount);
            m_cacheOwned = true;
        }
    }

    // Epoch must be GLOBALLY unique across every pool instance, not per-object
    // (SPEC_THUMB_DISPLAY 31/08 nghi van 3): each tab has its own pool, and a
    // fresh object always produced epoch==1, so a background tab's already-queued
    // thumbnail (posted just before the tab switch) matched the new tab's
    // m_acceptEpoch==1 and got accepted into the wrong list. A monotonic global
    // counter makes every open() distinct so stale cross-tab frames hit DROP stale.
    static std::atomic<quint64> s_epochSeq{0};
    m_epoch = ++s_epochSeq;
    for (int i = 0; i < kDocs; ++i) {
        if (i == 0) {
            if (prePageCount > 0)
                m_pageCount = prePageCount;
            else {
                TimedPdfiumLock thumbLock(__FILE__, __LINE__);
                m_pageCount = FPDF_GetPageCount(sharedDoc);
            }
        }

        auto* w = new ThumbnailWorker(sharedDoc, i, m_cache.get(), m_epoch, &m_renderPaused, this, nullptr);
        // 🔴 LƯỢT 31b (reviewer mục 5): reset tường minh theo dõi nhường nặng ở MỖI
        // open() — đổi tài liệu không được mang timestamp của tài liệu cũ sang.
        w->resetHeavyYields();
        // 0928 LƯỢT 16: tab mở nền (async) mà pool đã bị đóng băng trước đó ⇒ worker
        // sinh ra phải mang đúng trạng thái, không "rã băng" âm thầm.
        w->setFrozen(m_frozen);
        auto c = connect(w, &ThumbnailWorker::thumbnailReady, this,
                         [this](int pg, QImage img, quint64 ep, bool draft) {
                             qDebug() << "[perf] thumb POOL relay page=" << pg;
                             emit thumbnailReady(pg, img, ep, draft);
                         });
        if (!c)
            qDebug() << "[perf] thumb POOL RELAY CONNECT FAILED worker=" << (void*)w;
        else
            qDebug() << "[perf] thumb POOL relay connected worker=" << (void*)w << "pool=" << (void*)this;
        w->start();
        m_workers.push_back(w);
    }
    m_open = true;
    return true;
}

void ThumbnailRenderPool::close() {
    for (auto* w : m_workers) w->stop();
    for (auto* w : m_workers) { w->resetHeavyYields(); w->wait(); delete w; }   // L31b: reset khi dong
    m_workers.clear();
    // 0927 M2: chi release doi tuong pool TU acquire. Doi tuong cua tab do TAB
    // so huu (tab goi TileCacheRegistry::release khi dong tab) — neu pool xoa o
    // day thi tab con tab khac se mat dem giua chung.
    if (m_cacheOwned && m_cache) TileCacheRegistry::release(m_cache);
    m_cache.reset();
    m_cacheOwned = false;
    m_open = false;
    m_pageCount = 0;
    m_path.clear();
}

void ThumbnailRenderPool::setVisibleBand(int first, int last, int scrollVal, int scrollMax) {
    if (!m_open) return;
    for (auto* w : m_workers) w->setBand(first, last);
    // 0928 LƯỢT 15: vạch đỏ trong log — đây là lúc hàng chờ đổi bộ xương.
    // Trước lượt này log này không tồn tại nên không ai thấy hàng 350 phần tử
    // đứng trước mặt ô đang hiện.
    // 0928 LƯỢT 15b: kèm `scroll=<value>/<max>` để CEO đối chiếu vùng đo với
    // vị trí thanh cuộn thật — không có nó thì log `vung=0-3` mà thanh cuộn ở
    // 181–185 trông giống nhau, tức là không phân biệt được "đo sai" với "chưa
    // cuộn".
    qDebug().noquote() << QStringLiteral("[thumbq] cuon vung=%1-%2 scroll=%3/%4")
        .arg(first).arg(last).arg(scrollVal).arg(scrollMax);
}

void ThumbnailRenderPool::requestThumbnail(int pageIndex, int priority) {
    if (!m_open) { qDebug() << "[perf] thumb requestThumbnail skipped page=" << pageIndex << "pool not open"; return; }
    if (pageIndex < 0 || pageIndex >= m_pageCount) { qDebug() << "[perf] thumb requestThumbnail OOB page=" << pageIndex << "count=" << m_pageCount; return; }
    m_workers[pageIndex % kDocs]->enqueue(pageIndex, priority);
}

void ThumbnailRenderPool::prefetchRange(int first, int last) {
    if (!m_open) { qDebug() << "[perf] thumb prefetch SKIPPED — pool not open"; return; }
    int total = qMin(last, m_pageCount - 1) - first + 1;
    qDebug() << "[perf] thumb prefetch start total=" << total
             << "range=" << first << "-" << qMin(last, m_pageCount - 1);
    for (int i = first; i <= qMin(last, m_pageCount - 1); ++i)
        requestThumbnail(i, 2);
}
