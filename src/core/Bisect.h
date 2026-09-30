#pragma once
// ── 0927 LƯỢT 10 — CÔNG TẮC CHIA ĐÔI (CHỈ ĐỂ ĐO, KHÔNG SỬA KIẾN TRÚC) ────────────
// LƯỢT 9 đã khoá PDFium trên handle pool (--pool-lock) mà crash CHỈ giảm 5/10 → 3/10
// ⇒ tranh chấp pool KHÔNG phải (toàn bộ) gốc. Lượt này CHIA ĐÔI: tắt từng thành phần
// một (lớp bù chú thích / text page / pre-count / vector / thumbnail / quét chú thích /
// handle pool) để tìm ra thành phần nào giữ FPDF_PAGE của TÀI LIỆU CHÍNH lâu nhất.
//
// 🔴 HỢP ĐỒNG (không được phá):
//   1. MẶC ĐỊNH TẮT ⇒ HÀNH VI Y HƯỚC NAY, từng dòng logic không đổi.
//   2. CHỈ đặt biến + rẽ nhánh sớm. KHÔNG xoá hàm, KHÔNG đổi chữ ký, KHÔNG sửa thứ tự.
//   3. main() đặt biến TRƯỚC QApplication (CEO chạy Scheduled Task, khó đặt env)
//      rồi BỎ cờ khỏi argv. Cờ cộng dồn được, đứng ở bất kỳ vị trí nào.
//   4. Mỗi cờ bật ⇒ main() in đúng 1 dòng `[bisect] <ten>=1` (tên = cờ bỏ `--`).
//      Người chấm MỞ LOG, đòi dòng này; thiếu dòng ⇒ lượt chạy vô nghĩa.
// Không dùng `static` 1 lần như trPoolLockOn(): đọc trực tiếp để còn bật/tắt lúc
// chạy (đổi biến giữa chừng cũng có tác dụng) — qEnvironmentVariableIsEmpty rẻ.
#include <QString>

inline bool trBisect(const char* envName) {
    return !qEnvironmentVariableIsEmpty(envName);
}

// --no-fgn: không dựng/không đụng ForeignAnnotLayer (lớp bù chú thích ngoài).
inline bool trNoFgn()       { return trBisect("TORREADER_NO_FGN"); }
// --no-textpage: không FPDFText_LoadPage trên tài liệu chính.
inline bool trNoTextPage()  { return trBisect("TORREADER_NO_TEXTPAGE"); }
// --no-precount: bỏ heavycap pre-count (FPDFPage_CountObjects…).
inline bool trNoPreCount()  { return trBisect("TORREADER_NO_PRECOUNT"); }
// --no-vector: không dựng VectorLayer (kể cả đọc .torvec) — chỉ raster.
inline bool trNoVector()    { return trBisect("TORREADER_NO_VECTOR"); }
// --no-thumbs: không chạy ThumbnailRenderPool.
inline bool trNoThumbs()    { return trBisect("TORREADER_NO_THUMBS"); }
// --no-annotscan: không quét chú thích (comments / annotScan / annotVisuals).
inline bool trNoAnnotScan() { return trBisect("TORREADER_NO_ANNOTSCAN"); }
// --no-pool: PdfRenderer KHÔNG dùng pool handle — render mọi thứ trên tài liệu chính
// dưới s_pdfiumMutex (đường cũ).
inline bool trNoPool()      { return trBisect("TORREADER_NO_POOL"); }
// 0927 LƯỢT 11 — ĐẢO CHIỀU so với 7 cờ trên: mặc định là ĐÃ SỬA (thoát nhanh,
// không chạy hàm huỷ ⇒ né crash 0x80000003 lúc thoát). Cờ này BẬT = đường CŨ (chạy
// đủ hàm huỷ) để CEO còn tái hiện được crash khi đang truy gốc. Chi tiết vá:
// REPORT_CRASH_EYACHO_0927.md §"LƯỢT 11", cờ ở core/FastExit.h.
inline bool trThoatCham()   { return trBisect("TORREADER_THOAT_CHAM"); }

// Bảng cờ dòng lệnh → biến, dùng chung cho main() (đặt biến + in dòng xác nhận).
// Giữ một chỗ để không lệch giữa chỗ đặt biến và chỗ in log.
struct TrBisectFlag { const char* cli; const char* env; };
inline const TrBisectFlag* trBisectFlags(int* n) {
    static const TrBisectFlag kFlags[] = {
        {"--no-fgn",       "TORREADER_NO_FGN"},
        {"--no-textpage",  "TORREADER_NO_TEXTPAGE"},
        {"--no-precount",  "TORREADER_NO_PRECOUNT"},
        {"--no-vector",    "TORREADER_NO_VECTOR"},
        {"--no-thumbs",    "TORREADER_NO_THUMBS"},
        {"--no-annotscan", "TORREADER_NO_ANNOTSCAN"},
        {"--no-pool",      "TORREADER_NO_POOL"},
        // 0927 LƯỢT 11: bật = đường thoát CŨ (xem trThoatCham()).
        {"--thoat-cham",   "TORREADER_THOAT_CHAM"},
    };
    *n = int(sizeof(kFlags) / sizeof(kFlags[0]));
    return kFlags;
}
