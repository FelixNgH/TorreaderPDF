#pragma once
// ══════════════════════════════════════════════════════════════════════════════
// 🔴🔴 0928 LƯỢT 14 — SỔ VIỆC NỀN THEO TÀI LIỆU (vá use-after-free)
// ══════════════════════════════════════════════════════════════════════════════
// BẰNG CHỨNG (CEO đo trên máy test Windows, bản LƯỢT 13 + minidump —
// crash_dump_evidence_0927/stack_luot13_vectorbuild.txt, run_luot13_crash_2.txt):
//   • 4/4 minidump chết trên LUỒNG NỀN QtConcurrent, đúng chuỗi
//       StoredFunctionCall::runFunctor → VectorLayer::build → lambda_2 → pdfium
//   • `--thoat-cham` (đường huỷ đầy đủ) vẫn crash 6/12 lượt
//   • Log mọi lượt crash: `DOC-CLOSE-BEGIN … pages=1 texts=1 <<< CON SOT` rồi SAU
//     ĐÓ vẫn có `[khoa] giu ms=… tai=VectorLayer::build`
//
// ⇒ GỐC: đóng tài liệu (thoát app, đóng tab, nạp lại) KHÔNG huỷ và KHÔNG CHỜ việc
//   `VectorLayer::build` đang chạy. Nó còn giữ `PageBorrow` của tài liệu vừa
//   `FPDF_CloseDocument` ⇒ đọc vùng nhớ đã giải phóng.
//   (WSL offscreen không có GPU nên không bao giờ dựng vector ⇒ ASan WSL không thấy.)
//
// CÁCH SỬA: mỗi task nền chạm PDFium phải cầm một token `trdoc::Task` ngay khi
// bắt đầu. `PdfDocument::close()` gọi `beginClose()` TRƯỚC khi lấy khoá pdfium:
//   (a) bật cờ huỷ cho mọi task của doc → chúng dừng ở ranh giới lát kế tiếp;
//   (b) CHỜ có trần → nếu còn task thì KHÔNG đóng doc (rò còn hơn crash);
//   (c) mới `PageCache::forgetDocument` + `FPDF_CloseDocument`.
//
// ⛔ TUYỆT ĐỐI KHÔNG giữ `s_pdfiumMutex` trong lúc chờ — task cần khoá đó mới nhìn
//    thấy cờ huỷ, giữ khoá lúc chờ = tự khoá chết. Vì vậy `beginClose()` phải gọi
//    TRƯỚC dòng `BoundedPdfiumLock` trong `close()`.
//
// Đây là sổ ĐẾM (không phải QFuture): phần lớn task nền không giữ QFuture ở đâu,
// và nhiều task (`PageCache::prefetch`, `PdfLinks`, probe OCR) vốn đã bị bỏ rơi
// không thể chờ được. Bộ đếm + QWaitCondition là duy nhất phủ hết.
#include <QMutex>
#include <QWaitCondition>
#include <QDeadlineTimer>
#include <QHash>
#include <QElapsedTimer>
#include <QDebug>
#include <QStringList>
#include <atomic>
#include <chrono>
#include <memory>
#include <fpdfview.h>

namespace trdoc {

// Sổ toàn cục. `inline` + static cục bộ ⇒ MỘT bản duy nhất cho mọi TU.
struct Registry {
    QMutex m;
    QWaitCondition w;                                        // báo "việc vừa xong"
    QHash<FPDF_DOCUMENT, int> live;                          // task đang chạy, theo doc
    QHash<FPDF_DOCUMENT, std::shared_ptr<std::atomic<bool>>> flags;  // cờ huỷ, theo doc
    // loại việc → số đang chạy. Chỉ để log khi quá trần nói ra AI còn giữ doc.
    QHash<FPDF_DOCUMENT, QHash<QByteArray, int>> kinds;
};
inline Registry& reg() { static Registry r; return r; }

// Lấy (hoặc tạo) cờ huỷ của doc. shared_ptr ⇒ token giữ được dù bảng bị xoá.
inline std::shared_ptr<std::atomic<bool>> flagOf(FPDF_DOCUMENT doc) {
    if (!doc) return nullptr;
    QMutexLocker lk(&reg().m);
    auto it = reg().flags.find(doc);
    if (it == reg().flags.end())
        it = reg().flags.insert(doc, std::make_shared<std::atomic<bool>>(false));
    return it.value();
}

// ── Token: đặt ở ĐẦU THÂN mỗi task nền chạm PDFium của doc ────────────────────
class Task {
public:
    Task(FPDF_DOCUMENT doc, const char* kind = "task") : m_doc(doc), m_kind(kind) {
        if (!doc) return;
        m_flag = flagOf(doc);
        QMutexLocker lk(&reg().m);
        reg().live[m_doc] = reg().live.value(m_doc, 0) + 1;
        auto& k = reg().kinds[m_doc];
        const QByteArray key(m_kind);
        k[key] = k.value(key, 0) + 1;
    }
    // Vết huỷ theo lát: trả true ⇒ task dừng ở ranh giới lát kế tiếp (≤ 40 ms với
    // VectorLayer::build, kVecSliceMs). Dùng trong predicate `shouldCancel`.
    bool cancelled() const {
        return !m_flag || m_flag->load(std::memory_order_acquire);
    }
    ~Task() { release(); }
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    // 🔴 0928 LƯỢT 22 — MOVE có chủ đích: token ĐĂNG KÝ LÚC SPAWN (trên luồng UI,
    // TRƯỚC QtConcurrent::run) rồi chuyển quyền vào lambda:
    //     trdoc::Task task(d, "kind");                       // đếm NGAY khi xếp hàng
    //     fut = QtConcurrent::run([task = std::move(task)]{ ... task.cancelled() ... });
    // Để beginClose() thấy cả task ĐANG XẾP HÀNG chưa chạy (bản cũ đăng ký trong
    // thân task ⇒ cửa sổ "xếp hàng xong tab bị đóng" là UAF — reviewer LƯỢT 21 mục b).
    // Release TRỪ khi functor bị huỷ (task xong hoặc pool bỏ runnable) — không sớm hơn.
    Task(Task&& o) noexcept : m_doc(o.m_doc), m_kind(o.m_kind), m_flag(std::move(o.m_flag)) {
        o.m_doc = nullptr;
    }
    Task& operator=(Task&& o) noexcept {
        if (this != &o) { release();
            m_doc = o.m_doc; m_kind = o.m_kind; m_flag = std::move(o.m_flag);
            o.m_doc = nullptr; }
        return *this;
    }
private:
    void release() {
        if (!m_doc) return;
        QMutexLocker lk(&reg().m);
        const int left = reg().live.value(m_doc, 1) - 1;
        if (left > 0) {
            reg().live[m_doc] = left;
            auto& k = reg().kinds[m_doc];
            const QByteArray key(m_kind);
            const int kl = k.value(key, 1) - 1;
            if (kl > 0) k[key] = kl; else k.remove(key);
            if (k.isEmpty()) reg().kinds.remove(m_doc);
        } else {
            reg().live.remove(m_doc);
            reg().kinds.remove(m_doc);
        }
        reg().w.wakeAll();                    // đánh thức beginClose() đang chờ
        m_doc = nullptr;
    }
    FPDF_DOCUMENT m_doc = nullptr;
    const char*   m_kind = nullptr;
    std::shared_ptr<std::atomic<bool>> m_flag;
};

inline int inflight(FPDF_DOCUMENT doc) {
    if (!doc) return 0;
    QMutexLocker lk(&reg().m);
    return reg().live.value(doc, 0);
}

// Liệt kê loại việc còn giữ doc (dùng khi quá trần) — "VectorLayer::build x2,
// ocr x1 …" để biết ngay ai cần sửa.
inline QString liveKinds(FPDF_DOCUMENT doc) {
    QMutexLocker lk(&reg().m);
    QStringList out;
    const auto it = reg().kinds.constFind(doc);
    if (it != reg().kinds.constEnd())
        for (auto i = it->cbegin(); i != it->cend(); ++i)
            out << QString::fromLatin1(i.key()) + QStringLiteral(" x") + QString::number(i.value());
    return out.join(QLatin1Char(','));
}

// (a) Bật cờ huỷ cho MỌI việc nền của doc. Idempotent.
inline void cancelAll(FPDF_DOCUMENT doc) {
    if (auto f = flagOf(doc)) f->store(true, std::memory_order_release);
}

// (b) Chờ có trần mọi việc nền của doc. KHÔNG giữ s_pdfiumMutex.
//     trả false = quá trần, còn `*out` việc chưa xong (khi đó KHÔNG được đóng doc).
inline bool waitIdle(FPDF_DOCUMENT doc, int ms, int* out = nullptr, qint64* outMs = nullptr) {
    const int n0 = inflight(doc);
    QElapsedTimer t; t.start();
    int left = n0;
    while (left > 0 && t.elapsed() < ms) {
        QMutexLocker lk(&reg().m);
        left = reg().live.value(doc, 0);
        if (left <= 0) break;
        reg().w.wait(&reg().m, QDeadlineTimer(std::chrono::milliseconds(20)));
                                                              // 20 ms: nhả ra để bơm sự kiện UI
    }
    if (out)    *out   = left;
    if (outMs)  *outMs = t.elapsed();
    return left == 0;
}

// (a)+(b) một lần — gọi ngay TRƯỚC khi đóng tài liệu.
// `ms` = trần chờ. trả false ⇔ quá trần ⇒ người gọi PHẢI bỏ qua FPDF_CloseDocument.
inline bool beginClose(FPDF_DOCUMENT doc, int ms, const char* where) {
    if (!doc) return true;
    const int n = inflight(doc);
    if (n <= 0) { qDebug().noquote() << "[dongdoc] cho vec=0 ms=0 tai=" << where; return true; }
    cancelAll(doc);
    int left = n; qint64 waited = 0;
    const bool ok = waitIdle(doc, ms, &left, &waited);
    qDebug().noquote() << "[dongdoc] cho vec=" << n << " ms=" << waited << " tai=" << where;
    if (!ok)
        qCritical().noquote() << "[dongdoc] QUA TRAN " << ms << "ms — con " << left
                              << " viec nen [" << liveKinds(doc) << "], KHONG DONG doc tai="
                              << where << " (roi con hon crash)";
    return ok;
}

// Sau khi FPDF_CloseDocument: xoá sổ của doc. BẮT BUỘC — cùng một địa chỉ
// FPDF_DOCUMENT có thể được cấp lại cho lần mở sau, và nếu để lại cờ huỷ
// thì mọi task của lần mở sau sẽ tự hủy ngay.
inline void forget(FPDF_DOCUMENT doc) {
    if (!doc) return;
    QMutexLocker lk(&reg().m);
    reg().flags.remove(doc);
    reg().live.remove(doc);
    reg().kinds.remove(doc);
    reg().w.wakeAll();
}

}  // namespace trdoc
