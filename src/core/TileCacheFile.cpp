#include "TileCacheFile.h"
#include <QBuffer>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QHash>
#include <QMutexLocker>
#include <cstring>

static uint64_t computePdfHash(const QString& pdfPath) {
    QFile f(pdfPath);
    if (!f.open(QIODevice::ReadOnly)) return 0;
    qint64 fileSize = f.size();
    const qint64 kChunk = 65536;
    QByteArray ba1 = f.read(qMin(kChunk, fileSize));
    QByteArray ba2;
    if (fileSize > kChunk) {
        f.seek(fileSize - kChunk);
        ba2 = f.read(kChunk);
    }
    uint64_t h = static_cast<uint64_t>(fileSize);
    const uint64_t mul = 2654435761ULL;
    auto mix = [&](const QByteArray& b) {
        for (char c : b) h = (h * mul) ^ static_cast<uint8_t>(c);
    };
    // 🔴 LƯỢT 31 (A2 — reviewer bắt): trộn MTIME vào khoá. PDF đã Save ⇒ mtime đổi
    // ⇒ khoá mới ⇒ KHÔNG đọc lại ảnh cũ. Khoá cũ chỉ = size + 64KB đầu/cuối nên
    // sửa cùng kích thước ở GIỮA file (annotation trong object stream) ra cùng hash
    // ⇒ dùng nhầm ảnh cũ.
    const qint64 mtime = QFileInfo(pdfPath).lastModified().toMSecsSinceEpoch();
    for (int i = 0; i < 8; ++i) h = (h * mul) ^ static_cast<uint8_t>((mtime >> (i * 8)) & 0xFF);
    mix(ba1);
    mix(ba2);
    // 🔴 A2: hash THÊM các khối RẢI ĐỀU ở giữa file (8 mẫu × 4KB) ⇒ sửa ở giữa,
    // cùng size, cùng mtime (vd restore mtime) vẫn đổi khoá. Rẻ hơn hash toàn file.
    if (fileSize > 2 * kChunk) {
        const qint64 span = fileSize - 2 * kChunk;   // vùng giữa, ngoài đầu/cuối
        for (int k = 1; k <= 8; ++k) {
            f.seek(kChunk + (span * k) / 9);
            mix(f.read(4096));
        }
    }
    return h;
}

uint64_t TileCacheFile::hashFile(const QString& pdfPath) {
    return computePdfHash(pdfPath);
}

CacheZoom zoomBandFor(double s) {
    if (s < 0.5) return CacheZoom::Thumb;
    if (s < 1.5) return CacheZoom::Full;
    if (s < 2.5) return CacheZoom::Double;
    return CacheZoom::Quad;
}

double canonicalScale(CacheZoom band) {
    switch (band) {
        case CacheZoom::Thumb:  return 0.25;
        case CacheZoom::Full:   return 1.0;
        case CacheZoom::Double: return 2.0;
        case CacheZoom::Quad:   return 4.0;
    }
    return 1.0;
}

bool TileCacheFile::open(const QString& pdfPath, uint64_t pdfHash, uint64_t pdfSize, int pageCount) {
    QMutexLocker lock(&m_mutex);
    if (m_open) { m_file.close(); m_open = false; }
    m_path = pdfPath;
    const QString cachePath = cachePathFor(pdfPath, pdfHash);
    m_cachePath = cachePath;

    // 0927 M1: khoa tường minh truoc KHI mo file. That bai = co instance/tab
    // khac dang giu file nay (hoac app khac chua thoat het — QLockFile doc PID
    // trong file .lock). Ta KHONG ghi de: doi tuong nay bo qua dia (RAM only)
    // cho an toan, vi ghi chung 1 file tu 2 process se lam hong blob PNG.
    m_lock = std::make_unique<QLockFile>(cachePath + ".lock");
    if (!m_lock->tryLock(0)) {
        qDebug().noquote() << QString("[torcache] BO QUA %1 ly do=lockKhac").arg(cachePath);
        m_lock.reset();
        return false;
    }

    if (QFile::exists(cachePath)) {
        m_file.setFileName(cachePath);
        if (m_file.open(QIODevice::ReadWrite)) {
            if (m_file.size() >= (qint64)sizeof(TorCacheHeader)) {
                m_file.seek(0);
                m_file.read(reinterpret_cast<char*>(&m_header), sizeof(TorCacheHeader));

                bool valid = std::strncmp(m_header.magic, "TORCACH7", 8) == 0
                          && m_header.pdfHash   == pdfHash
                          && m_header.pdfSize   == pdfSize
                          && m_header.pageCount == (uint32_t)pageCount;
                if (valid) {
                    m_pageCount = pageCount;
                    m_index.resize(pageCount * 4);
                    qint64 need = (qint64)m_index.size() * sizeof(TorEntry);
                    qint64 got  = m_file.read(reinterpret_cast<char*>(m_index.data()), need);
                    if (got == need) {
                        m_open = true;
                        return true;
                    }
                }
            }
            m_file.close();
        }
        QFile::remove(cachePath);
    }

    // Create fresh cache
    m_file.setFileName(cachePath);
    if (!m_file.open(QIODevice::ReadWrite | QIODevice::Truncate)) {
        m_lock->unlock();                    // 0927 M1: fail thi tra khoa lai
        m_lock.reset();                      // 0927 M6: `unlock()` da tu xoa .lock
        return false;                        //   nen bo QFile::remove(.lock) thuong
    }

    std::memset(&m_header, 0, sizeof(m_header));
    std::memcpy(m_header.magic, "TORCACH7", 8);
    m_header.pdfHash    = pdfHash;
    m_header.pdfSize    = pdfSize;
    m_header.pageCount  = static_cast<uint32_t>(pageCount);
    m_header.zoomLevels = 4;

    m_file.write(reinterpret_cast<char*>(&m_header), sizeof(m_header));

    m_pageCount = pageCount;
    m_index.fill(TorEntry{}, pageCount * 4);
    m_file.write(reinterpret_cast<char*>(m_index.data()), (qint64)m_index.size() * sizeof(TorEntry));
    m_file.flush();
    m_open = true;
    return true;
}

void TileCacheFile::close() {
    QMutexLocker lock(&m_mutex);
    if (m_open) { m_file.close(); m_open = false; }
}

// 🔴 LƯỢT 30 (VIỆC 2, C): như close() nhưng NHẢ QLockFile và KHÔNG xoá file.
// QLockFile::unlock() tự xoá file .lock của nó; file .torcache ở lại trên đĩa.
void TileCacheFile::closeKeepFile() {
    QMutexLocker lock(&m_mutex);
    if (m_open) { m_file.close(); m_open = false; }
    if (m_lock) { m_lock->unlock(); m_lock.reset(); }
}

bool TileCacheFile::closeAndRemove(QString* error) {
    // 0927: dong handle cua chinh minh truoc, roi moi xoa — va BAO LOI that bai.
    // 0927 M1/M2: nha QLockFile o day. Chi doi tuong NAY duoc xoa khi khong con
    // ai dung (registry dem so tab) — xoa qua `close()` truc tiep se lam TAB KHAC
    // mat dem giua chung.
    // 0927 M6: THU TU co y nghia — xoa .torcache TRUOC, nha khoa SAU. Nha khoa
    // truoc thi giua hai buoc instance khac lay duoc khoa va mo dung chinh file
    // sap bi xoa (Linux `unlink` lam mat dem cua no luon).
    const QString path = m_cachePath;
    close();
    if (error) error->clear();
    if (path.isEmpty()) return true;              // chua mo bao gio → khong co file
    bool ok = true;
    if (QFile::exists(path)) {                    // chua xoa (vd app da chet) → xoa
        QFile f(path);
        ok = f.remove();
        if (!ok && error) *error = f.errorString();
    }
    // 0927 M6: `QLockFile::unlock()` TU xoa file .lock, va KHONG xoa khi file da
    // thuoc instance khac ⇒ bo `QFile::remove(.lock)` thuong (lenh do con gay xoa
    // nham .lock vua duoc ban khac tao). `m_lock` chi khac null khi chinh ta da
    // giu khoa nen goi unlock() o day an toan.
    if (m_lock) { m_lock->unlock(); m_lock.reset(); }
    return ok;
}

QImage TileCacheFile::readPage(int pageIndex, CacheZoom zoom) {
    QMutexLocker lock(&m_mutex);
    if (!m_open || pageIndex < 0 || pageIndex >= m_pageCount) return {};
    int idx = entryIndex(pageIndex, zoom);
    if (idx >= m_index.size() || m_index[idx].status != 1) return {};
    const TorEntry& e = m_index[idx];
    m_file.seek((qint64)e.offset);
    QByteArray blob = m_file.read((qint64)e.size);
    return QImage::fromData(blob, "PNG");
}

bool TileCacheFile::writePage(int pageIndex, CacheZoom zoom, const QImage& image) {
    QByteArray blob;
    {
        QBuffer buf(&blob);
        buf.open(QIODevice::WriteOnly);
        image.save(&buf, "PNG", 1);
    }
    if (blob.isEmpty()) return false;

    QMutexLocker lock(&m_mutex);
    if (!m_open || pageIndex < 0 || pageIndex >= m_pageCount) return false;

    qint64 off = m_file.size();
    m_file.seek(off);
    m_file.write(blob.constData(), blob.size());

    int idx = entryIndex(pageIndex, zoom);
    // 🔴 LƯỢT 31 (B6): ghi CANH DAI that cua anh vao 3 byte `pad` (trước đây bỏ
    // trống). Lúc MỞ LẠI, requestFromCacheOnlyForContinuous so ảnh đọc được với
    // CHÍNH kích thước đã lưu — thay vì đoán lại fullQCapPx từ pageObjectCount=0
    // (trang quái vật bị coi là NHẸ ⇒ đòi 4000px ⇒ VUT entry 357px ⇒ vẽ lại 9s).
    const int longEdge = qMax(image.width(), image.height());
    const uint32_t le  = static_cast<uint32_t>(qMin(longEdge, 0xFFFFFF));
    TorEntry e{ static_cast<uint64_t>(off), static_cast<uint32_t>(blob.size()), 1,
                { static_cast<uint8_t>(le & 0xFF),
                  static_cast<uint8_t>((le >> 8) & 0xFF),
                  static_cast<uint8_t>((le >> 16) & 0xFF) } };
    m_index[idx] = e;

    m_file.seek(entryFileOffset(pageIndex, zoom));
    m_file.write(reinterpret_cast<char*>(&m_index[idx]), sizeof(TorEntry));
    m_file.flush();
    return true;
}

// 🔴 LƯỢT 31 (B6): canh dai ANH DA LUU cho (page, zoom) — 0 = khong biet (entry
// cu / chua ghi). Doc tu m_index (RAM), khong cham file.
int TileCacheFile::storedLongPx(int pageIndex, CacheZoom zoom) const {
    QMutexLocker lock(&m_mutex);
    if (!m_open || pageIndex < 0 || pageIndex >= m_pageCount) return 0;
    int idx = entryIndex(pageIndex, zoom);
    if (idx >= m_index.size() || m_index[idx].status != 1) return 0;
    const uint8_t* p = m_index[idx].pad;
    return int(p[0]) | (int(p[1]) << 8) | (int(p[2]) << 16);
}

bool TileCacheFile::hasPage(int pageIndex, CacheZoom zoom) const {
    QMutexLocker lock(&m_mutex);
    if (!m_open || pageIndex < 0 || pageIndex >= m_pageCount) return false;
    int idx = entryIndex(pageIndex, zoom);
    return idx < m_index.size() && m_index[idx].status == 1;
}

void TileCacheFile::invalidatePage(int pageIndex) {
    QMutexLocker lock(&m_mutex);
    if (!m_open || pageIndex < 0 || pageIndex >= m_pageCount) return;
    int cleared = 0;
    for (int z = 0; z < 4; ++z) {
        CacheZoom zoom = static_cast<CacheZoom>(z);
        int idx = entryIndex(pageIndex, zoom);
        if (idx < m_index.size() && m_index[idx].status != 0) {
            m_index[idx] = TorEntry{};
            m_file.seek(entryFileOffset(pageIndex, zoom));
            m_file.write(reinterpret_cast<char*>(&m_index[idx]), sizeof(TorEntry));
            ++cleared;
        }
    }
    if (cleared > 0) {
        m_file.flush();
        qDebug().noquote() << "[cache] disk invalidate page=" << pageIndex << "entries=" << cleared;
    }
}

// ─── 0927 M1+M2: TileCacheRegistry ───────────────────────────────────────────
// CHI goi tu UI thread (MainWindow: mo tab / dong tab / thoat app / don mo coi,
// va ThumbnailRenderPool::open/close cung tu UI thread) ⇒ khong co khoa o day.
// `open()` cua TileCacheFile van tu khoa m_mutex nen ghi van an toan.
namespace {
struct Entry {
    std::shared_ptr<TileCacheFile> tc;
    int users = 0;
};
QHash<QString, Entry> g_registry;
} // namespace

std::shared_ptr<TileCacheFile> TileCacheRegistry::acquire(const QString& pdfPath, uint64_t pdfHash,
                                                           uint64_t pdfSize, int pageCount) {
    const QString key = TileCacheFile::cachePathFor(pdfPath, pdfHash);
    // 0927 LƯỢT 6: mo lai tai lieu trong chinh phien nay = duoc phep dung cache
    // lai. `release()` da CAM ghi .torvec cua key nay khi rut cache; neu khong go
    // o day thi mo lai PDF trong cung phien se khong bao gio ghi duoc .torvec
    // (cache RAM van chay, nhung moi lan phai dung lai PDF).
    VectorCache::unbanKey(VectorCache::keyFromCachePath(key));
    auto it = g_registry.find(key);
    if (it != g_registry.end()) {            // 0927 M2: 2 tab cung 1 PDF → 1 doi tuong
        ++it->users;
        qDebug().noquote() << QString("[torcache] DUNG CHUNG %1 tab=%2").arg(key).arg(it->users);
        return it->tc;
    }
    auto tc = std::make_shared<TileCacheFile>();
    if (!tc->open(pdfPath, pdfHash, pdfSize, pageCount)) {
        // Khoa khong chay duoc (instance khac) → tra doi tuong CHUA mo. Caller
        // dung `isOpen()` de biet bo qua dia, nhung ma van giu shared_ptr de
        // khong phai doi xu ly null o moi noi.
        return tc;
    }
    g_registry.insert(key, Entry{tc, 1});
    return tc;
}

TileCacheRegistry::Release TileCacheRegistry::release(const std::shared_ptr<TileCacheFile>& tc,
                                                       const char* reason) {
    Release r;
    if (!tc) return r;
    r.path = tc->cachePath();
    auto it = g_registry.find(r.path);
    if (it == g_registry.end()) {            // da duoc xoa o noi khac, hoac chua mo duoc
        r.ok = true;
        return r;
    }
    // 0927 M6: SO CON TRO, khong so duong dan. `open()` gan `m_cachePath` TRUOC
    // khoa ⇒ mot `tc` mo that bai (lockKhac) van mang `m_cachePath = X` ma KHONG
    // bao gio vao registry. Sau do handle KHAC (tab2) acquire thanh cong va
    // dang ky `tc2` cho X. `release(tc1)` se tim ra entry cua tc2, giam users ve 0
    // va xoa dem cua tab2 DANG DUNG. Khac con tro ⇒ tra som y het nhu "khong co".
    if (it->tc != tc) { r.ok = true; return r; }
    r.stillUsed = --it->users;
    if (r.stillUsed > 0) return r;           // con tab khac dung → GIU file
    // 🔴 LƯỢT 30 (VIỆC 2, C): GIỮ .torcache + .torvec qua đóng/mở lại TRONG PHIÊN.
    // Chủ nhân: mở lại file ⇒ hiện ngay từ cache (≤1 s), không vẽ lại 9 s. Mặc định
    // GIỮ; TORREADER_PURGE_ON_CLOSE=1 khôi phục hành vi cũ (xoá) để A/B.
    if (!qEnvironmentVariableIsSet("TORREADER_PURGE_ON_CLOSE")) {
        it->tc->closeKeepFile();             // dong handle + nha khoa, GIU file
        r.ok = true; r.removed = false; r.kept = true;
        g_registry.erase(it);
        qDebug().noquote() << QString("[torcache] GIU (dong tab, giu file) %1").arg(r.path);
        return r;
    }
    QString err;
    r.ok      = it->tc->closeAndRemove(&err);
    r.error   = err;
    r.removed = r.ok;
    g_registry.erase(it);
    // 0927 LƯỢT 6: `.torvec` cua cung tai lieu DI CUNG VON DOI (mo ta trong
    // .h). Khong sua o 3 noi goi `release()` — ca 3 deu rut cache theo cung
    // quy tac, nen rut luon o day cho khop mot lan. `purgeKey` CAM ghi truoc khi
    // xoa ⇒ task .torvec dang chay se bo file .tmp cua no, khong ghi lai.
    VectorCache::purgeKey(VectorCache::keyFromCachePath(r.path), reason);
    return r;
}

TileCacheRegistry::RemoveResult TileCacheRegistry::removeIfUnlocked(const QString& cachePath,
                                                                    QString* error) {
    if (error) error->clear();
    if (cachePath.isEmpty()) return RemoveResult::NotFound;
    // 0927 M1: `tryLock(0)` = "co ai dang giu file nay khong?". QLockFile doc PID
    // trong file .lock: app da chet ⇒ lock mo coi, ta nhan lai ⇒ thanh cong.
    // That bai ⇒ con process dang dung ⇒ KHONG duoc xoa (Linux unlink luon
    // thanh cong nen truong hop nay phai co khoa tuong minh).
    // 0927 M6: khoa TRUOC roi kiem tra .torcache — nho do .lock mo coi con lai
    // (khong con .torcache) cung duoc don bang chinh `unlock()` nay.
    QLockFile lock(cachePath + ".lock");
    if (!lock.tryLock(0)) return RemoveResult::Locked;
    if (!QFile::exists(cachePath)) {               // da sach roi → don .lock roi
        lock.unlock();
        return RemoveResult::NotFound;
    }
    QFile f(cachePath);
    const bool ok = f.remove();                    // xoa TRUOC khi nha khoa
    // 0927 M6: bo `QFile::remove(.lock)` thuong (QLockFile::unlock() tu xoa, va
    // khong xoa nham khi .lock da thuoc instance khac).
    lock.unlock();
    if (!ok && error) *error = f.errorString();
    return ok ? RemoveResult::Removed : RemoveResult::Failed;
}
