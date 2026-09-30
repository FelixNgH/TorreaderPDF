#pragma once
#include <QMutex>
#include <QElapsedTimer>
#include <QThread>
#include <QCoreApplication>
#include <QDebug>
#include <QAtomicInteger>
#include <atomic>
#include <optional>

extern QMutex s_pdfiumMutex;

// ══════════════════════════════════════════════════════════════════════════════
// 🔴 0928 LƯỢT 13 — HÀNG CHỜ CÓ MỨC ƯU TIÊN, VẪN MỘT KHOÁ CHUNG.
// ══════════════════════════════════════════════════════════════════════════════
// Đo trên máy test (file 350 trang A1 256 MB, log hardtest_evidence_0928/
// run_log_khoachung_nhay239.txt): End + PgUp×111 tới trang 239 mất ~17 s với
// khoá chung, < 5 s với `--pool-song-par`. Log cho thấy KHÔNG phải khoá chậm —
// mà là 26 `VectorLayer::build` cho các trang người dùng ĐÃ LƯỚT QUA (349, 348,
// 347 … 324) xếp hàng TRƯỚC trang đang xem, mỗi cái giữ khoá 0,5–7 s theo lát.
//   [lockwait] ms= 2905 at VectorLayer::build acquire   (trang ĐANG XEM, chờ)
//   [lockwait] ms= 2346 at PdfRenderer.cpp:480         (render trang đang xem)
//
// Không mở khoá (đã chứng minh hỏng — xem khối 0927 LƯỢT 12 bên dưới). Thay vào
// đó: việc NÀO cần khoá thì VIẾT vào số đếm `g_pdfiumUrgent`; việc NỀN (thumbnail,
// OCR probe) thấy số đếm > 0 thì LÙI LẠI thay vì giành khoá. Đây là cách duy
// nhất làm được với QMutex — không có API xếp hàng theo ưu tiên, chỉ có "ai
// nhường".
//
//   uuTien 0 = trang đang hiển thị (đường raster + vector của trang đó)
//   uuTien 1 = lân cận / prefetch nền
//   uuTien 2 = thumbnail, OCR probe, annot scan  ← phải nhường
inline std::atomic<int>& g_pdfiumUrgentCounter() {
    static std::atomic<int> n{0};
    return n;
}
inline bool pdfiumUrgentPending() { return g_pdfiumUrgentCounter().load(std::memory_order_acquire) > 0; }

// 🔴 LƯỢT 33f (mục 1 — reviewer): nhường cho việc khẩn KHÁC, không nhường cho chính
// mình. Task trang hiển thị tự tăng bộ đếm (UrgentPdfiumScope) rồi loop chờ bộ đếm
// về 0 ⇒ ngủ trọn trần 2 s vô ích (hồi quy tốc độ 33e: trang 3 +3,9 s, trang 8 +4,0 s).
// `ownShare` = phần bộ đếm do chính task này đóng góp (1 nếu nó là trang hiển thị).
inline bool pdfiumUrgentPendingBeyond(int ownShare) {
    return g_pdfiumUrgentCounter().load(std::memory_order_acquire) > ownShare;
}

// 🔴 0928 LƯỢT 18 (VIỆC 2): trang ma view chinh DANG CHO ANH (primary, chua co anh).
// Do log r17: thumbnail cung trang giu khoa 1335 ms (FPDF_LoadPage) trong luc render
// trang do dung chai — hai luot cung mot trang thi mot phai sau. ContinuousView ghi
// (GUI thread) khi doi trang / xoa khi ACCEPT; worker thumbnail doc va nhuong trang
// ay. -1 = khong ai cho. An toan da tab: hang doi thumbnail chi chay cho tab dang
// xem (L16 dong bang tab nen) va gia tri cu bi tran nhoi doi 5 s o phia doc.
inline std::atomic<int>& g_pdfiumUrgentPage() {
    static std::atomic<int> p{-1};
    return p;
}

// 🔴 0928 LƯỢT 19 (CEO 18.7-ii): moc thoi diem primary BAT DAU cho anh (ms he
// thong, 0 = khong ai cho). ContinuousView ghi khi dat g_pdfiumUrgentPage;
// thumbnail doc de tinh TRAN 5 s — tinh TU LUC CHO, khong phai moi request —
// qua tran thi ve binh thuong (chong doi o).
inline std::atomic<qint64>& g_pdfiumUrgentPageDeadline() {
    static std::atomic<qint64> t{0};
    return t;
}

// 🔴 LƯỢT 33f (mục 2 — reviewer): cửa sổ "trang đầu chờ ảnh" chỉ chặn trang hiển thị
// của tab KHÁC. Không có token doc thì trang hiển thị của CHÍNH tab bị trang phụ
// cùng tab chặn (đo 33e: L tab load, trang hiển thị nhường trọn 5 s vì up != page).
// Ghi đồng thời với g_pdfiumUrgentPage. 0 = tab ĐANG MỞ (chưa có handle) — vẫn là
// "doc khác" với mọi tab đã load ⇒ giữ nguyên đường nhường J của 33e.
inline std::atomic<uintptr_t>& g_pdfiumUrgentDoc() {
    static std::atomic<uintptr_t> d{0};
    return d;
}

class UrgentPdfiumScope {
public:
    explicit UrgentPdfiumScope(bool on) : m_on(on) {
        if (m_on) g_pdfiumUrgentCounter().fetch_add(1, std::memory_order_acq_rel);
    }
    ~UrgentPdfiumScope() {
        if (m_on) g_pdfiumUrgentCounter().fetch_sub(1, std::memory_order_acq_rel);
    }
    UrgentPdfiumScope(const UrgentPdfiumScope&) = delete;
    UrgentPdfiumScope& operator=(const UrgentPdfiumScope&) = delete;
private:
    bool m_on;
};

// Log MỖI lát giữ khoá >= 50 ms. Yêu cầu 0928 việc 6:
//   [khoa] giu ms=<n> tai=<ham> uuTien=<muc> trang=<n>
// `trang` = -1 khi nơi gọi không biết trang nào (mặc định mọi call site cũ).
inline void logKhoaSlice(qint64 heldMs, const char* ham, int trang) {
    if (heldMs < 50) return;   // dưới ngưỡng này thì lách cách, không đáng in
    const int uu = pdfiumUrgentPending() ? 0 : 2;
    qDebug().noquote() << "[khoa] giu ms=" << heldMs << "tai=" << ham
                       << "uuTien=" << uu << "trang=" << trang;
}

// ── BO DEM VONG DOI PDFIUM (bai do ro ri bo nho 2026-08-31) ────────────────
// Do duoc: mo roi DONG han tai lieu ma RSS khong tra ve (102MB -> giu lai 2.294MB).
// Dem MO vs DONG de biet cai gi khong duoc giai phong.
#include <QAtomicInteger>
extern QAtomicInteger<int> g_pdfiumDocOpen, g_pdfiumDocClose;
extern QAtomicInteger<int> g_pdfiumPoolOpen, g_pdfiumPoolClose;
extern QAtomicInteger<int> g_pdfiumPageOpen, g_pdfiumPageClose;
// Byte texture GPU dang cap phat (bai do 31/08). Texture nam trong bo nho driver —
// KHONG xuat hien trong bo dem nao cua app nhung CO tinh vao RSS tien trinh.
extern QAtomicInteger<qint64> g_gpuTexBytes;
extern QAtomicInteger<int>    g_gpuTexAlive;

// Do CA hai phia cua khoa pdfium: thoi gian CHO lay khoa va thoi gian GIU khoa.
// Nguong 300 ms cho [lockwait]. Danh dau ro luot nao chay tren LUONG CHINH (main=1) vi do la
// luot lam dung hinh giao dien.
class TimedPdfiumLock {
    const char* m_file;
    int         m_line;
    int         m_page = -1;   // 0928: trang nao dang xin khoa (-1 = khong biet)
    QElapsedTimer m_hold;
    bool        m_locked = true;
public:
    TimedPdfiumLock(const char* file, int line, int page = -1)
        : m_file(file), m_line(line), m_page(page) {
        QElapsedTimer w; w.start();
        s_pdfiumMutex.lock();
        const qint64 waited = w.elapsed();
        m_hold.start();
        const bool isMain = (QThread::currentThread() == QCoreApplication::instance()->thread());
        // 🔴 TIEU CHI SO 1 CUA OWNER (31/08): KHONG BAO GIO duoc not-responding.
        // Dinh nghia do duoc: LUONG GIAO DIEN khong duoc di xin khoa pdfium — bat ky lan nao.
        // Bat bang TORREADER_GUILOCK=1.
        static const bool kGuiLockAudit = !qEnvironmentVariableIsEmpty("TORREADER_GUILOCK");
        if (isMain && kGuiLockAudit)
            qDebug().noquote() << "[guilock] cho=" << waited << "ms at" << m_file << ":" << m_line;
        if (waited > 300) {
            qDebug().noquote() << "[lockwait] ms=" << waited << "at" << m_file << ":" << m_line
                               << "main=" << (isMain ? 1 : 0) << "trang=" << m_page;
        }
    }
    void unlock() {
        if (!m_locked) return;
        const qint64 held = m_hold.elapsed();
        s_pdfiumMutex.unlock();
        m_locked = false;
        logKhoaSlice(held, m_file, m_page);
        if (held > 300)
            qDebug().noquote() << "[lockhold] ms=" << held << "at" << m_file << ":" << m_line;
    }
    ~TimedPdfiumLock() { unlock(); }
    TimedPdfiumLock(const TimedPdfiumLock&) = delete;
    TimedPdfiumLock& operator=(const TimedPdfiumLock&) = delete;
};

// Non-blocking lock for GUI thread (SPEC_SMOOTH_123 VIỆC 1).
// - On GUI thread: tryLock(0). If fail → held()==false, caller must return stale
//   data and dispatch real work to background thread.
// - On non-GUI thread: normal blocking lock (held() always true).
// Lấy được thì làm; KHÔNG lấy được thì trả dữ liệu cũ + đẩy sang luồng nền.
class TryPdfiumLock {
    const char* m_file;
    int         m_line;
    int         m_page = -1;
    bool        m_held = false;
    QElapsedTimer m_hold;
public:
    TryPdfiumLock(const char* file, int line, int page = -1)
        : m_file(file), m_line(line), m_page(page) {
        const bool isMain = (QThread::currentThread() == QCoreApplication::instance()->thread());
        static const bool kGuiLockAudit = !qEnvironmentVariableIsEmpty("TORREADER_GUILOCK");
        if (isMain) {
            m_held = s_pdfiumMutex.tryLock(0);
            if (kGuiLockAudit)
                qDebug().noquote() << "[guilock] cho= 0 ms at" << m_file << ":" << m_line;
            if (m_held) m_hold.start();
        } else {
            s_pdfiumMutex.lock();
            m_held = true;
            m_hold.start();
        }
    }
    bool held() const { return m_held; }
    void unlock() {
        if (!m_held) return;
        const qint64 held = m_hold.elapsed();
        s_pdfiumMutex.unlock();
        m_held = false;
        logKhoaSlice(held, m_file, m_page);
        if (held > 300)
            qDebug().noquote() << "[lockhold] ms=" << held << "at" << m_file << ":" << m_line;
    }
    ~TryPdfiumLock() { unlock(); }
    TryPdfiumLock(const TryPdfiumLock&) = delete;
    TryPdfiumLock& operator=(const TryPdfiumLock&) = delete;
};

// Non-blocking lock for GUI thread with a BOUNDED retry, for operations that
// MUST complete (doc close) but must not hold the GUI hostage forever.
// - GUI thread: tryLock(0) up to `maxRetries`×2ms (~50ms); if still held, it is
//   a real lockholder (render slice) that will release soon → fall back to one
//   blocking lock (bounded, no livelock). 
// - Non-GUI thread: plain blocking lock.
class BoundedPdfiumLock {
    const char* m_file;
    int         m_line;
    int         m_page = -1;
    bool        m_held = false;
    QElapsedTimer m_hold;
public:
    BoundedPdfiumLock(const char* file, int line, int maxRetries = 25, int page = -1)
        : m_file(file), m_line(line), m_page(page) {
        const bool isMain = (QThread::currentThread() == QCoreApplication::instance()->thread());
        static const bool kGuiLockAudit = !qEnvironmentVariableIsEmpty("TORREADER_GUILOCK");
        if (isMain) {
            for (int i = 0; i < maxRetries; ++i) {
                if (s_pdfiumMutex.tryLock(0)) { m_held = true; break; }
                QThread::msleep(2);
            }
            if (kGuiLockAudit)
                qDebug().noquote() << "[guilock] cho=" << (m_held ? 0 : maxRetries)
                                   << "ms at" << m_file << ":" << m_line;
            if (!m_held) {
                qDebug().noquote() << "[lockwait] bounded-gui blocked at" << m_file << ":" << m_line;
                s_pdfiumMutex.lock();
                m_held = true;
            }
            if (m_held) m_hold.start();
        } else {
            s_pdfiumMutex.lock();
            m_held = true;
            m_hold.start();
        }
    }
    bool held() const { return m_held; }
    void unlock() {
        if (!m_held) return;
        const qint64 held = m_hold.elapsed();
        s_pdfiumMutex.unlock();
        m_held = false;
        logKhoaSlice(held, m_file, m_page);
        if (held > 300)
            qDebug().noquote() << "[lockhold] ms=" << held << "at" << m_file << ":" << m_line;
    }
    ~BoundedPdfiumLock() { unlock(); }
    BoundedPdfiumLock(const BoundedPdfiumLock&) = delete;
    BoundedPdfiumLock& operator=(const BoundedPdfiumLock&) = delete;
};

// ══════════════════════════════════════════════════════════════════════════════
// 🔴🔴 0927 LƯỢT 12 — KHOÁ CHUNG PDFium LÀ MẶC ĐỊNH VĨNH VIỄN. ĐỪNG MỞ LẠI.
// ══════════════════════════════════════════════════════════════════════════════
// LƯỢT 9 bật khoá pool bằng CỜ THỬ NGHIỆM (TORREADER_POOL_LOCK=1 / --pool-lock) và
// chỉ giảm crash 5/10 → 3/10, nên LƯỢT 10 đi tìm thành phần khác. NHƯNG CEO đã dựng
// được pdfium 7906 tự build ASan + TSan (crash_dump_evidence_0927/) và đo được:
//
//   • ASan — heap-use-after-free trong `CPDF_Color::~CPDF_Color` khi FPDF_ClosePage.
//     Đối tượng bị thả là ColorSpace TOÀN CỤC (CPDF_ColorSpace::InitializeGlobals /
//     GetStockCS) dùng chung MỌI FPDF_DOCUMENT; RetainPtr đếm KHÔNG nguyên tử.
//   • TSan — 229 tranh chấp dính pdfium, giữa PageCache.cpp:524/280/705 (tài liệu
//     chính) ↔ ThumbnailRenderPool.cpp:152/245 ↔ PdfRenderer.cpp:367/425.
//   • Bật --pool-lock: TSan 0 tranh chấp pdfium, ASan 0/6 lỗi (không khoá: 2/3).
//
// ⇒ KẾT LUẬN KHÔNG PHẢI "tranh chấp pool", mà là: **PDFIUM KHÔNG AN TOÀN ĐA LUỒNG
// KỂ CẢ KHI HAI FPDF_DOCUMENT KHÁC NHAU**. Handle pool không tạo vùng bộ nhớ riêng
// cho trạng thái toàn cục; nó chỉ tách cấu trúc riêng theo handle. Nên "render song
// song trên handle pool" là một giả định SAI, và cờ A/B chỉ che nó đi.
//
// Vì vậy hành vi MẶC ĐỊNH ĐÃ ĐẢO: mọi lời gọi PDFium — MỌI LUỒNG, MỌI TÀI LIỆU,
// kể cả trên handle pool — đều giữ s_pdfiumMutex. Cờ thử nghiệm cũ bị gỡ; thay bằng
// cờ NGƯỢC `--pool-song-par` / TORREADER_POOL_SONGPAR, MẶC ĐỊNH TẮT, CHỈ để CEO tái
// hiện lại crash khi đang truy gốc.
//
// ⛔ Cờ `--pool-song-par` = CỐ TÌNH BẬT LẠI ĐƯỜNG KHÔNG KHOÁ ĐÃ BIẾT LÀ HỎNG.
// Chỉ dùng để tái hiện, KHÔNG bao giờ bật ở bản phát hành. Đừng "tắt khoá cho nhanh"
// rồi đóng gói — đó chính là nguồn 0x80000003 + heap 0xc0000374 mà 4/4 minidump
// 27/09 đã chứng minh.
//
// VẪN NHẢ KHOÁ GIỮA CÁC LÁT: lát Start và từng lát Continue đều khoá riêng, nhả
// trước mỗi lần emit/copy QImage. Giao diện không đứng — nhịp này y hệt nhánh
// PageCache vốn đã chạy ổn.
//
// Đọc 1 lần, static — main() đặt biến TRƯỚC QApplication, trước khi luồng nền chạy.
inline bool trPoolLockOn() {
    static const bool on =
        qEnvironmentVariableIsEmpty("TORREADER_POOL_SONGPAR");
    return on;
}

// Giữ khoá PDFium CÓ ĐIỀU KIỆN. Mặc định (không có cờ ngược) ⇒ y hệt
// TimedPdfiumLock, tức LUÔN khoá. Chỉ rỗng khi CEO bật --pool-song-par để tái hiện.
// `unlock()` tay để trả khoá giữa hai lát progressive mà không phải cắt khối.
class MaybePdfiumLock {
    std::optional<TimedPdfiumLock> m_lk;
public:
    MaybePdfiumLock(const char* file, int line, bool on, int page = -1) {
        if (on) m_lk.emplace(file, line, page);
    }
    bool held() const { return m_lk.has_value(); }
    void unlock() { m_lk.reset(); }
    ~MaybePdfiumLock() = default;
    MaybePdfiumLock(const MaybePdfiumLock&) = delete;
    MaybePdfiumLock& operator=(const MaybePdfiumLock&) = delete;
};
