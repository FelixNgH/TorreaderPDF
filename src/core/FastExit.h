#pragma once
// ── 0927 LƯỢT 11 — CỜ "PHẦN BẮT BUỘC KHI THOÁT ĐÃ XONG" (vá tạm) ─────────────
// Crash 0x80000003 lúc thoát (UI → PageCache::closeEntry → FPDF_ClosePage, ~50%
// lượt với file CAD nặng) nằm trong hàm huỷ. Vá: sau khi `app.exec()` trả về thì
// KHÔNG chạy hàm huỷ nào nữa, huỷ thẳng tiến trình — nhưng chỉ khi chắc mọi việc
// người dùng cần đã xong. Mọi việc đó nằm trong MainWindow::closeEvent (hỏi lưu,
// lưu, dừng pool, rút cache + nhả QLockFile), nên:
//   closeEvent chạy xong  → closeEvent đặt cờ ở đây  → main() đọc cờ ngay sau
//   app.exec()            → được phép huỷ tiến trình.
// Không có cờ này thì "mọi việc đã xong" chỉ là tin đoán. Đường thoát KHÔNG đi qua
// closeEvent (qApp->quit() trong GateDialog, probe tự `QCoreApplication::quit()`)
// ⇒ cờ = false ⇒ rơi về đường cũ, y hành vi hôm nay.
#include <QElapsedTimer>

// true ⇔ MainWindow::closeEvent đã chạy hết phần bắt buộc (không phải đường Cancel).
inline bool& trExitReadyRef() { static bool v = false; return v; }
inline bool trExitReady()      { return trExitReadyRef(); }

// Đồng hồ "phần bắt buộc xong lúc nào" — để log `sauDonMs=` (app còn sống thêm bao
// lâu sau khi xong việc, tức trước khi bị huỷ).
inline QElapsedTimer& trExitDoneClock() { static QElapsedTimer t; return t; }

// Gọi ĐÚNG MỘT LẦN, cuối MainWindow::closeEvent (sau e->accept()).
inline void trMarkExitReady() {
    trExitReadyRef() = true;
    trExitDoneClock().start();
}

// Chính sách bản nháp `.tortmp` (bản PDF nháp khi sửa trang) lúc thoát nhanh:
//   0 = chưa biết (closeEvent chưa chạy)          → coi như KHÔNG xoá
//   1 = được xoá: không có gì chưa lưu, hoặc user trả lời "Không" ở hộp thoát
//   2 = TUYỆT ĐỐI giữ: user vừa bấm "Có" (lưu) mà lưu hỏng ⇒ bản nháp là bản DUY
//       NHẤT còn lại. ~MainWindow (đường cũ) xoá vô điều kiện; thoát nhanh thì không.
inline int& trExitDraftPolicy() { static int v = 0; return v; }
