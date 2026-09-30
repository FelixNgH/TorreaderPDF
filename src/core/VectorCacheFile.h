#pragma once
#include <QString>
#include <QtGlobal>

class VectorLayer;

// Cache vector heavy-page (.torvec). SPEC_PERF_HEAVYPAGE buoc B.
// 0927 LƯỢT 6: SỐNG TRONG PHIÊN — .torvec ĐI CHUNG VÒNG ĐỜI VỚI .torcache.
//   Trước đây .torvec SỐNG MÃI trên đĩa (chỉ có `enforceBudget` dọn theo 1,5 GB)
//   nên đóng PDF xong vẫn còn file của PDF đó trong %TEMP% — chính là thứ owner
//   phản đối. Nay mỗi lần registry xoá .torcache của tài liệu (tab cuối đóng /
//   thoát app / tải lại) thì .torvec + .torvec.tmp của tài liệu đó đi luôn.
namespace VectorCache {
    // ── KHOA CHUNG (một nguồn suy nhất) ──────────────────────────────────────
    // key = <tênfile>_<hash16>. Mọi tên file cache đều ghép từ key:
    //   .torcache      = key + ".torcache"          (TileCacheFile::cachePathFor)
    //   .torvec        = key + "_p<N>.torvec"       (pathFor)
    //   ban ghi tam    = key + "_p<N>.torvec.tmp"   (trySave)
    QString keyFor(const QString& pdfPath, quint64 pdfHash);
    // Nguồc lại 1: cắt đuôi ".torcache" (khi đi từ đường dẫn .torcache sang key).
    QString keyFromCachePath(const QString& torcachePath);
    // Nguồc lại 2: cắt đuôi "_p<N>.torvec[.tmp]" (khi dọn mồ côi lúc khởi động).
    QString keyFromVecFileName(const QString& fileName);

    // Duong dan file cache cho (pdf, trang). Cung thu muc voi .torcache:
    //   QDir::temp()/<tenfile>_<hash16>_p<N>.torvec
    QString pathFor(const QString& pdfPath, quint64 pdfHash, int pageIndex);

    // Doc lop vector tu cache. Tra false neu khong co file / hong / sai phien ban.
    // Khong giu s_pdfiumMutex (chi doc file). Kiem pageIndex khop sau loadFrom.
    bool tryLoad(VectorLayer& out, const QString& pdfPath, quint64 pdfHash, int pageIndex);

    // Ghi lop vector ra cache (atomic: file tam .tmp roi rename). TU CHOI ghi khi:
    //   - layer khong ready/complete
    //   - layer.approxBytes() > kMaxPerPageBytes
    //   - cho trong tren o dia < kMinFreeBytes  (bytesAvailable() < 0 => TU CHOI)
    //   - tai lieu dang bi `banKey` (cache vua rut) => bo file .tmp cua minh
    // Sau khi ghi thanh cong goi enforceBudget() de don cache cu (TRAN TRONG PHIEN).
    bool trySave(const VectorLayer& layer, const QString& pdfPath, quint64 pdfHash, int pageIndex);

    // Don cache: xoa file .torvec cu nhat cho toi khi tong <= kMaxTotalBytes.
    void enforceBudget();

    // ── 0927 LƯỢT 6: RÚT CACHE THEO VÒNG ĐỜI CỦA .torcache ─────────────────
    // `purgeKey` CẤM GHI key TRƯỚC khi xoá: task ghi .torvec đang chạy (hoặc sẽ
    // chạy sau) thấy cấm thì bỏ .tmp của mình ⇒ không có .tmp mồ côi, không có
    // file nào được TẠO LẠI sau khi đã xoá. `unbanKey` gỡ cấm khi tài liệu được
    // mở lại trong chính phiên này (mở lại = được dựng cache mới).
    int  purgeKey(const QString& key, const char* reason);   // tra so file da xoa that
    void banKey(const QString& key);
    void unbanKey(const QString& key);
    int  countForKey(const QString& key);   // so file cua tai lieu con tren dia (TCPROBE)

    constexpr qint64 kMaxPerPageBytes = 300LL * 1024 * 1024;   // 300 MB / trang
    constexpr qint64 kMaxTotalBytes   = 1500LL * 1024 * 1024;  // 1,5 GB tong
    constexpr qint64 kMinFreeBytes    = 3LL * 1024 * 1024 * 1024; // con < 3 GB thi KHONG ghi
}
