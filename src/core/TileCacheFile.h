#pragma once
#include <QString>
#include <QImage>
#include <QVector>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QMutex>
#include <QStringList>
#include "VectorCacheFile.h"
#include <memory>
#include <cstdint>

enum class CacheZoom : uint8_t { Thumb=0, Full=1, Double=2, Quad=3 };

CacheZoom zoomBandFor(double scaleFactor);
double canonicalScale(CacheZoom band);

// Disk cache — stores per-page PNG images. Magic bump to TORCACH6 invalidates
// old caches from the pre-tiling era. Per-tile disk caching is deferred (the
// in-memory tile cache in PdfRenderer handles the primary working set).
#pragma pack(push, 1)
struct TorCacheHeader {
    char     magic[8];       // "TORCACH7" — 2026-09-29 LƯỢT 31 (B6): nang tu 6 len 7 vi
                             // 3 byte `pad` cua TorEntry bay gio chua CANH DAI anh da luu
                             // (storedLongPx). Cache cu (pad=0, kieu "TORCACH6") tu dong bi
                             // coi la khong hop le va dung lai — khong doc nham du lieu cu.
    uint64_t pdfHash;
    uint64_t pdfSize;
    uint32_t pageCount;
    uint8_t  zoomLevels;     // =4
    uint8_t  reserved[35];   // 64 - 8 - 8 - 8 - 4 - 1 = 35
};
static_assert(sizeof(TorCacheHeader) == 64, "TorCacheHeader must be 64 bytes");

struct TorEntry {
    uint64_t offset;   // 0 = not rendered
    uint32_t size;
    uint8_t  status;   // 0=empty 1=ready 2=error
    // 🔴 LƯỢT 31 (B6): 3 byte nay = CANH DAI anh da luu (little-endian 24-bit),
    //   truoc day bo trong. Xem TileCacheFile::storedLongPx.
    uint8_t  pad[3];
};
static_assert(sizeof(TorEntry) == 16, "TorEntry must be 16 bytes");
#pragma pack(pop)

class TileCacheFile {
public:
    static uint64_t hashFile(const QString& pdfPath);
    // 0927 M1/M2: ten file .torcache la MOT NGUON SUY NHAT — registry va moi noi
    // khoa deu di qua day. Tinh sẵn de ca hai noi KHONG tinh lai (va de khoa
    // khop dang ke ca o duong open that bai).
    // 0927 LƯỢT 6: `<key>.torcache` — key do CHINH VectorCache::keyFor sinh ra,
    // dung chung ca .torvec. Truoc day hai noi tu ghep ten bang hai bieu thuc
    // khac nhau: lech mot ky tu la .torvec cua PDF no khong con "cua" PDF nay
    // ⇒ rut cache khong xoa het. Bay ca hai cùng goi keyFor.
    static QString cachePathFor(const QString& pdfPath, uint64_t pdfHash) {
        return QDir::temp().filePath(VectorCache::keyFor(pdfPath, pdfHash)
                                     + QStringLiteral(".torcache"));
    }

    bool open(const QString& pdfPath, uint64_t pdfHash, uint64_t pdfSize, int pageCount);
    void close();
    bool isOpen() const { return m_open; }

    QString cachePath() const { return m_cachePath; }
    // 0927: tra true NEU DA XOA THAT. Windows khoa viec xoa file khi bat ky handle
    // nao con mo (ERROR_SHARING_VIOLATION) ⇒ lenh xoa that bai trong im lang. Do do
    // phai tra ket qua + errorString cho caller log trung thuc.
    // 0927 M1: nha handle, xoa file, ROI nha QLockFile + xoa file .lock.
    bool closeAndRemove(QString* error = nullptr);
    // 🔴 LƯỢT 30 (VIỆC 2, C): đóng handle + nhả QLockFile nhưng GIỮ file .torcache
    // trên đĩa, để mở lại cùng file (trong phiên) đọc thẳng từ cache ⇒ hiện ≤1 s,
    // không vẽ lại từ đầu. Header validate hash+size+pageCount nên file cũ không
    // khớp sẽ tự bị bỏ qua; startup cleanup vẫn dọn file mồ côi của app đã chết.
    void closeKeepFile();

    QImage readPage(int pageIndex, CacheZoom zoom);
    bool writePage(int pageIndex, CacheZoom zoom, const QImage& image);
    bool hasPage(int pageIndex, CacheZoom zoom) const;
    // 🔴 LƯỢT 31 (B6): canh dai ANH DA LUU (ghi vao 3 byte pad luc writePage).
    // 0 = khong biet. Dung luc MỞ LẠI de chap nhan anh bang dung kich thuoc no
    // duoc luu, khong doan lai fullQCapPx tu pageObjectCount=0.
    int storedLongPx(int pageIndex, CacheZoom zoom) const;
    void invalidatePage(int pageIndex);

private:
    mutable QMutex     m_mutex;
    int entryIndex(int page, CacheZoom z) const { return page*4 + (int)z; }
    qint64 entryFileOffset(int page, CacheZoom z) const {
        return (qint64)sizeof(TorCacheHeader) + entryIndex(page,z)*(qint64)sizeof(TorEntry);
    }

    QString            m_path;
    QString            m_cachePath;   // 0927 M1: ten file .torcache da bi khoa
    // 0927 M1: <cache>.lock — "tao nay dang mo". QLockFile (Qt6) chi nhan ten
    // o ham dung, khong co setter ⇒ phai giu qua con tro.
    std::unique_ptr<QLockFile> m_lock;
    QFile              m_file;
    int                m_pageCount = 0;
    TorCacheHeader     m_header{};
    QVector<TorEntry>  m_index;
    bool               m_open = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// 0927 M1+M2 — REGISTRY: MOT file .torcache = MOT DOI TUONG TileCacheFile trong
// process, va file do CHI bi xoa khi khong con chu.
//
// Ly do (2 loi người chấm CHANGES_REQUESTED):
//   M2: `ThumbnailRenderPool` + `PdfRenderer` mo HAI doi tuong tren CUNG mot
//       duong dan .torcache, moi doi tuong giu `m_index` rieng va ghi append tai
//       `m_file.size()` rieng ⇒ de ghi de blob cua nhau.
//   M1: `cleanupOrphanTorcache` xoa thang moi .torcache trong
//       temp, chi trong vao he dieu hanh (Windows co the chan, Linux thi unlink
//       LUON thanh cong) ⇒ instance 2 khoi dong xoa dung dem cua instance 1.
//
// Giai phap: mot QLockFile `<cache>.lock` banh sat moi file .torcache, giu suot
// phien. Noi xoa chi chay khi `tryLock(0)` THANH CONG (QLockFile tu nhan lock
// mo coi theo PID ⇒ thanh cong = khong con instance nao chu). Registry dem
// bao nhieu tab dung, nen 2 tab cung mo 1 PDF chi co MOT doi tuong; tab cuoi
// cung roi moi xoa file.
class TileCacheRegistry {
public:
    struct Release {
        QString path;
        bool    ok       = true;   // false = lenh xoa that bai that
        bool    removed  = false;  // true = file da bien mat khoi dia
        bool    kept     = false;  // 🔴 LƯỢT 31 (A2): true = GIU file (dong tab, mac dinh)
                                   //   — KHONG phai "xoa ok=0", log phai noi "GIU".
        int     stillUsed = 0;     // >0: van con tab/doi tuong khac dung
        QString error;
    };
    enum class RemoveResult { Removed, NotFound, Locked, Failed };

    // Moi noi muon dung dia (tab, pool) chi giu shared_ptr, KHONG tu goi open().
    static std::shared_ptr<TileCacheFile> acquire(const QString& pdfPath, uint64_t pdfHash,
                                                  uint64_t pdfSize, int pageCount);
    // Nha tab. Van con ai dung ⇒ chi giam dem. Het nguoi ⇒ dong + xoa file +
    // nha khoa (mien moi doi tuong duoc xoa trong `open()`) + rut `.torvec` +
    // `.torvec.tmp` cua cung tai lieu. `reason` chi de ghi nhan trong log
    // `[torvec] XOA ok=.. <path> ly do=<reason>` (dongTab/thoatApp/taiLai).
    static Release release(const std::shared_ptr<TileCacheFile>& tc,
                           const char* reason = "ngoaiRoiTaiLieu");

    // Xoa 1 file .torcache bat ke (dung cho don mo coi) — CHI
    // khi khoa cua no khong con chu. Tra Locked = dang co instance khac dung.
    static RemoveResult removeIfUnlocked(const QString& cachePath, QString* error = nullptr);
};
