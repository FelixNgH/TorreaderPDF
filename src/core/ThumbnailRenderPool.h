#pragma once
#include <QObject>
#include <QImage>
#include <QMutex>
#include <QVector>
#include <QHash>
#include <QWaitCondition>
#include <QThread>
#include <QAtomicInt>
#include <atomic>
#include <fpdfview.h>
#include "TileCacheFile.h"

// 🔴 0928 LƯỢT 15c — BỎ `std::priority_queue`, GIỮ LẠI BẢNG XẾP HÀNG.
// LƯỢT 15 dùng heap nhị phân với comparator đọc 2 atomic `m_bandLo/m_bandHi`:
// đổi vùng là ĐỔI kết quả so sánh, mà `priority_queue` chỉ giữ thứ tự MỘT PHẦN,
// nên phải dựng lại cả heap (`setBand`) — và bản thân việc dựng lại đó KHÔNG đảm
// bảo thứ tự nữa vì nó rút phần tử bằng `top()/pop()` với comparator đã đổi.
// Đo được trong log 15b: hàng còn 3 yêu cầu `uu=0` (trang 152–154) + 2 yêu cầu
// `uu=1` ngay cạnh chúng, mà worker vẫn bốc ra trang 74 `uu=2` rồi bỏ đi, lặp
// 63→147 trong 14 s ⇒ không bao giờ tới vùng đang hiện.
// Nay hàng là `QVector` và thứ tự được TÍNH LẠI lúc lấy việc (`takeNext`), không
// có heap để vỡ, không có bản ghi trùng để phải xoá lười. Quy tắc (LƯỢT 15c):
//   uu 0 = trang ĐANG HIỆN, lấy theo thứ tự trên→dưới (số trang nhỏ trước)
//   uu 1 = lân cận ±kAdjacentDist quanh vùng
//   uu 2 = nền, CHỈ được lấy khi hàng không còn uu 0/1; trong tầng nền chọn
//          trang GẦN vùng nhất, không đi tuần tự từ đầu file.
struct ThumbRequest {
    int pageIndex = -1;
    int priority = 2;   // 0=visible (highest), 1=adjacent, 2=background
    int yieldTries = 0; // ponytail: counter for yield-back attempts per request
    // Đã bị CẮT NGẮN một lần (bản nháp do quá trần thời gian, hoặc bỏ vì đã xa
    // khỏi vùng đang xem). Lần sau cứ render cho trọn, không cắt nữa — đây là
    // mỏng chặn duy nhất chống lặp vô hạn.
    bool provisional = false;
};

class ThumbnailRenderPool;  // Forward declaration (VIỆC: ThumbnailWorker cần pool handle)

class ThumbnailWorker : public QThread {
    Q_OBJECT
public:
    ThumbnailWorker(FPDF_DOCUMENT doc, int slot, TileCacheFile* cache, quint64 epoch,
                    std::atomic<bool>* renderPaused, ThumbnailRenderPool* pool, QObject* parent = nullptr);
    void stop();
    void enqueue(int pageIndex, int priority = 2);
    // 🔴 0928 LƯỢT 16 — ĐÓNG BĂNG TAB NỀN. Trả true khi trạng thái THỰC SỰ đổi
    // (để caller in log `doi tab dong bang` đúng một lần mỗi lần đổi tab).
    // Frozen = worker KHÔNG bốc việc: hàng giữ nguyên, không log `lay trang`,
    // không đụng PDFium ⇒ tab nền không giành khoá chung với tab đang xem.
    // Bỏ băng (về tab cũ) thì hàng còn đó, bốc tiếp từ đầu.
    bool setFrozen(bool f);
    // 0928 LƯỢT 15: vùng các ô ĐANG HIỂN TRÊN KHUNG thumbnail. Mọi việc nền
    // ngoài vùng này bị đẩy xuống cuối hàng theo cự ly (thay vì đi từ cuối file).
    void setBand(int first, int last);
    // 🔴 LƯỢT 31b (reviewer mục 5): xoa theo doi nhuong — pool goi trong open()/close(),
    // moc thoi gian cua tai lieu cu KHONG duoc sovang sang tai lieu moi.
    void resetHeavyYields() { QMutexLocker lock(&m_mutex); m_heavySkipSince.clear(); }

signals:
    // `draft` = ảnh CHƯA render xong (quá trần thời gian). Panel dùng nó cho ô
    // thumbnail nhưng KHÔNG đưa vào `lowResPageAvailable` — bản nháp làm
    // ContinuousView tưởng trang đó "đã có gi để nhìn" ⇒ bỏ lượt render thật.
    void thumbnailReady(int pageIndex, QImage image, quint64 epoch, bool draft);
protected:
    void run() override;
private:
    // 🔴 0928 LƯỢT 15c — HÀNG CHỜ KHÔNG PHẢI HEAP. Một `QVector` theo thứ tự xếp
    // và `takeNext()` quét tìm việc tốt nhất theo vùng ĐANG HIỂN. 500 phần tử quét
    // mất vài micro-giây, worker 150 ms mới lấy một việc — đổi lại không còn
    // invariant nào để giữ, không còn comparator đọc atomic, không còn bản ghi
    // trùng phải xoá lười. Ưu tiên KHÔNG lưu trong hàng: nó tính lại ở `takeNext`
    // nên đổi vùng là tự xếp lại, không cần `make_heap` thủ công.
    FPDF_DOCUMENT m_doc;
    int  m_slot;
    TileCacheFile* m_cache;
    quint64        m_epoch = 0;
    std::atomic<bool>* m_renderPaused = nullptr; // pool's pause flag (may be null for safety)
    ThumbnailRenderPool* m_pool = nullptr;  // VIỆC: để gọi pdfRenderer() lấy pool handle
    std::atomic<int> m_bandLo{0};
    std::atomic<int> m_bandHi{-1};
    QVector<ThumbRequest> m_queue;  // hàng chờ (thứ tự mảng = thứ tự xếp ⇒ FIFO khi khoá bằng)
    // Việc NỀN bị cắt giữa chừng (giữ cờ `provisional` = render trọn lần sau).
    // KHÔNG xếp lại ngay (đó là vòng quay vô ích ở LƯỢT 15: 63→147 bỏ dở từng
    // trang) — chỉ gỡ lại khi hàng không còn uu 0/1.
    QVector<ThumbRequest> m_parked;
    // 🔴 0928 LƯỢT 16 — đóng băng bốc việc (tab nền). Đọc/ghi dưới `m_mutex`.
    bool           m_frozen = false;
    // 🔴 LƯỢT 31 (A1) + LƯỢT 31b (reviewer mục 5): trang NANG (>=1M object) CHUA co
    // anh chinh ⇒ NHUONG, nhưng tra tran theo THOI GIAN (ms lan dau gap trang),
    // khong theo dem luot — ba luot requestVisibleThumbnails lien luc mo tab
    // tung vuot tran dem cu (3) truoc khi anh chinh ve ⇒ pool parse lai 2,18M
    // object. Qua kHeavyThumbYieldMs thi VE THAT 1 lan o o thumbnail (luoi chong
    // o trang). Doc/ghi duoi m_mutex. Pool goi resetHeavyYields() trong open()/
    // close() ⇒ khong mang timestamp tai lieu cu sang tai lieu moi.
    QHash<int,qint64> m_heavySkipSince;
    static constexpr qint64 kHeavyThumbYieldMs = 10000;
    QMutex         m_mutex;
    QWaitCondition m_cond;
    bool           m_stop = false;
    // ±N trang quanh vùng đang xem được tính là "lân cận" (uu 1). Ngoài N là
    // nền (uu 2), và một việc nền đang chạy mà xa hơn N thì bị cắt. MỘT hằng số
    // cho cả hai: việc được nhận với uu 1 thì chắc chắn còn trong N, nên nhánh
    // "bỏ vì xa" không bao giờ cắt nhầm việc lân cận.
    static constexpr int kAdjacentDist = 32;
    int bandDist(int page) const {
        const int l = m_bandLo.load(std::memory_order_relaxed);
        const int h = m_bandHi.load(std::memory_order_relaxed);
        if (h < l) return 0;
        if (page < l) return l - page;
        if (page > h) return page - h;
        return 0;
    }
    // 0 = đang hiện, 1 = lân cận, 2 = nền. Tính từ vùng hiện tại mỗi lần gọi.
    int tierFor(int page) const {
        const int l = m_bandLo.load(std::memory_order_relaxed);
        const int h = m_bandHi.load(std::memory_order_relaxed);
        if (h < l) return 0;                              // chưa biết vùng
        if (page >= l && page <= h) return 0;
        return (bandDist(page) <= kAdjacentDist) ? 1 : 2;
    }
    // Bốc việc tốt nhất. Trả false = hết việc. Gỡ `m_parked` chỉ khi hàng không
    // còn uu 0/1 (luật "nền xa không được chen vào việc đang hiện").
    bool takeNext(ThumbRequest* out);
    // Trả việc về cuối hàng sau khi nhường/tạm dừng — không nhân bản.
    void requeueLocked(const ThumbRequest& r) {
        for (const auto& x : m_queue)
            if (x.pageIndex == r.pageIndex) return;
        m_queue.push_back(r);
        m_cond.wakeOne();
    }
};

class ThumbnailRenderPool : public QObject {
    Q_OBJECT
public:
    static constexpr double kThumbScale = 0.15;
    static constexpr int kDocs = 1;

    explicit ThumbnailRenderPool(QObject* parent = nullptr);
    ~ThumbnailRenderPool() override;

    // Mo pool tren CUNG FPDF_DOCUMENT voi renderer chinh (SPEC_PERF_HEAVYPAGE R1): khong
    // tu FPDF_LoadDocument nua de khong parse lai lan thu hai. Pool KHONG dong handle nay —
    // chi tai lieu chu (PdfDocument) moi FPDF_CloseDocument. sharedDoc phai song tu khi
    // open() toi khi close(). Giu nguyen epoch + disk cache. preHash/preSize = hash+size da
    // tinh san (tranh hash lai ca file tren UI thread); 0 = tinh noi bo nhu cu.
    // prePageCount = so trang da biet san (0 = tu hoi PDFium nhu cu). Ly do: hoi PDFium
    // phai xep hang khoa toan cuc, dung hinh giao dien.
    // 0927 M2: `tileCache` (tham so CUOI, mac dinh null) = doi tuong TileCacheFile DO
    // TAB so huu. Truoc day pool tu `m_cache.open()` mot doi tuong rieng tren CUNG
    // duong dan .torcache voi tab ⇒ hai doi tuong ghi append bang `m_file.size()`
    // rieng, de de danh nhau lam hong blob PNG. `writePage` da co mutex ben trong
    // nen dung chung MOT doi tuong la an toan. NULL (cac bench/test o main.cpp) ⇒
    // pool tu acquire trong TileCacheRegistry va tu release luc close().
    bool open(const QString& pdfPath, FPDF_DOCUMENT sharedDoc,
              uint64_t preHash = 0, uint64_t preSize = 0, int prePageCount = 0,
              std::shared_ptr<TileCacheFile> tileCache = nullptr);
    void close();
    // R3: nhan thumbnail dung SAN tu ben ngoai (ve bang GPU tu lop vector) — ghi vao
    // cache dia va phat ra nhu mot thumbnail binh thuong, KHONG cham PDFium.
    void insertThumbnail(int pageIndex, const QImage& img);
    // 🔴 LƯỢT 36 (mục 2): .torcache ĐÃ có ô thumbnail này ⇒ worker sẽ tự phát từ đĩa
    // (đo r36: step N 20 lap, worker "thumb cache hit" 58 lan — GPU path UI là thừa).
    bool hasCachedThumb(int pageIndex) const {
        return m_cache && m_cache->isOpen() && m_cache->hasPage(pageIndex, CacheZoom::Thumb);
    }
    bool isOpen()    const { return m_open; }
    int  pageCount() const { return m_pageCount; }

    void requestThumbnail(int pageIndex, int priority = 2);
    void prefetchRange(int first, int last);
    // 0928 LƯỢT 15: báo vùng các ô ĐANG HIỂN trong khung thumbnail (đã tính cả
    // hàng dưới). Mọi worker xếp lại hàng theo vùng này ngay ở ranh giới việc
    // kế tiếp — không phải đợi hàng rỗng.
    // 0928 LƯỢT 15b: `scrollVal/scrollMax` chỉ để in ra log đối chiếu, không
    // ảnh hưởng xếp hàng.
    void setVisibleBand(int first, int last, int scrollVal = -1, int scrollMax = -1);
    // 🔴 0928 LƯỢT 16 — ĐÓNG BĂNG CẢ POOL (tab nền / đang đóng tab). Trả true khi
    // trạng thái THỰC SỰ đổi. Luật mới của lượt này: thumbnail CHỈ CHO TAB ĐANG XEM.
    // Bản 08-31 bỏ tạm-dừng tab nền vì PAUSE không RESUME được — ở đây hàng đợi
    // KHÔNG bị xoá và bỏ băng là bốc tiếp ngay, nên "tab nền đứng lại" không còn
    // làm mất thumbnail.
    bool setFrozen(bool f);
    const std::vector<ThumbnailWorker*>& workers() const { return m_workers; }
    quint64 epoch() const { return m_epoch; }

    // Tam dung viec RENDER bang PDFium (doc cache dia van chay binh thuong).
    // Dung khi trang nguoi dung dang xem chua san sang — thumbnail khong duoc
    // gianh s_pdfiumMutex voi no. Mac dinh false.
    void setRenderPaused(bool paused);
    bool isRenderPaused() const;

    // VIỆC THUMBNAIL SONG SONG: truy cập PdfRenderer pool handle.
    // MainWindow gọi sau khi mở tab để cho thumbnail dùng pool handle render không khoá.
    void setPdfRenderer(class PdfRenderer* renderer);
    class PdfRenderer* pdfRenderer() const { return m_pdfRenderer; }

signals:
    void thumbnailReady(int pageIndex, QImage image, quint64 epoch, bool draft);

private:
    std::vector<ThumbnailWorker*> m_workers;
    // 0927 M2: shared_ptr, KHONG phai gia tri — worker giu con tro tho `.get()`
    // nen shared_ptr phai song it nhat den khi moi worker bi delete (close()).
    std::shared_ptr<TileCacheFile> m_cache;
    bool   m_cacheOwned = false;   // true = pool tu acquire ⇒ close() phai release
    quint64 m_epoch     = 0;
    bool    m_open      = false;
    int     m_pageCount = 0;
    QString m_path;
    std::atomic<bool> m_renderPaused{false};
    bool   m_frozen   = false;   // 0928 LƯỢT 16: trạng thái đóng băng của cả pool
    class PdfRenderer* m_pdfRenderer = nullptr;  // VIỆC: pool handle từ renderer
};
