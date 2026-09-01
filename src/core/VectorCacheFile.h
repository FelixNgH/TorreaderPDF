#pragma once
#include <QString>
#include <QtGlobal>

class VectorLayer;

// Disk cache cho lop vector heavy-page (.torvec). SPEC_PERF_HEAVYPAGE buoc B.
// Key: thu muc QDir::temp(), ten gan hash 16 ky tu + so trang. Tay don bat buoc
// vi mot trang co the len den ~200-300 MB (KHONG viet ra thu muc du an/OneDrive).
namespace VectorCache {
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
    // Sau khi ghi thanh cong goi enforceBudget() de don cache cu.
    bool trySave(const VectorLayer& layer, const QString& pdfPath, quint64 pdfHash, int pageIndex);

    // Don cache: xoa file .torvec cu nhat cho toi khi tong <= kMaxTotalBytes.
    void enforceBudget();

    constexpr qint64 kMaxPerPageBytes = 300LL * 1024 * 1024;   // 300 MB / trang
    constexpr qint64 kMaxTotalBytes   = 1500LL * 1024 * 1024;  // 1,5 GB tong
    constexpr qint64 kMinFreeBytes    = 3LL * 1024 * 1024 * 1024; // con < 3 GB thi KHONG ghi
}
