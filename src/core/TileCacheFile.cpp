#include "TileCacheFile.h"
#include <QBuffer>
#include <QDir>
#include <QFileInfo>
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
    for (char c : ba1) h = (h * mul) ^ static_cast<uint8_t>(c);
    for (char c : ba2) h = (h * mul) ^ static_cast<uint8_t>(c);
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
    QString cachePath = QDir::temp().filePath(
        QFileInfo(pdfPath).fileName() + "_" + QString::number(pdfHash, 16) + ".torcache");

    if (QFile::exists(cachePath)) {
        m_file.setFileName(cachePath);
        if (m_file.open(QIODevice::ReadWrite)) {
            if (m_file.size() >= (qint64)sizeof(TorCacheHeader)) {
                m_file.seek(0);
                m_file.read(reinterpret_cast<char*>(&m_header), sizeof(TorCacheHeader));

                bool valid = std::strncmp(m_header.magic, "TORCACH2", 8) == 0
                          && m_header.pdfHash   == pdfHash
                          && m_header.pdfSize   == pdfSize
                          && m_header.pageCount == (uint32_t)pageCount;
                if (valid) {
                    m_pageCount = pageCount;
                    m_index.resize(pageCount * 4);
                    qint64 need = (qint64)m_index.size() * sizeof(TileEntry);
                    qint64 got  = m_file.read(reinterpret_cast<char*>(m_index.data()), need);
                    if (got == need) {
                        m_open = true;
                        m_jpegQuality = m_header.jpegQuality;
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
    if (!m_file.open(QIODevice::ReadWrite | QIODevice::Truncate)) return false;

    std::memset(&m_header, 0, sizeof(m_header));
    std::memcpy(m_header.magic, "TORCACH2", 8);
    m_header.pdfHash    = pdfHash;
    m_header.pdfSize    = pdfSize;
    m_header.pageCount  = static_cast<uint32_t>(pageCount);
    m_header.zoomLevels = 4;
    m_header.jpegQuality = 85;

    m_file.write(reinterpret_cast<char*>(&m_header), sizeof(m_header));

    m_pageCount = pageCount;
    m_index.assign(pageCount * 4, TileEntry{});
    m_file.write(reinterpret_cast<char*>(m_index.data()), (qint64)m_index.size() * sizeof(TileEntry));
    m_file.flush();
    m_open = true;
    m_jpegQuality = 85;
    return true;
}

void TileCacheFile::close() {
    QMutexLocker lock(&m_mutex);
    if (m_open) { m_file.close(); m_open = false; }
}

QImage TileCacheFile::readPage(int pageIndex, CacheZoom zoom) {
    QMutexLocker lock(&m_mutex);
    if (!m_open || pageIndex < 0 || pageIndex >= m_pageCount) return {};
    int idx = entryIndex(pageIndex, zoom);
    if (idx >= m_index.size() || m_index[idx].status != 1) return {};
    const TileEntry& e = m_index[idx];
    m_file.seek((qint64)e.offset);
    QByteArray blob = m_file.read((qint64)e.size);
    return QImage::fromData(blob, "JPG");
}

bool TileCacheFile::writePage(int pageIndex, CacheZoom zoom, const QImage& image) {
    QByteArray blob;
    {
        QBuffer buf(&blob);
        buf.open(QIODevice::WriteOnly);
        image.save(&buf, "JPG", m_jpegQuality);
    }
    if (blob.isEmpty()) return false;

    QMutexLocker lock(&m_mutex);
    if (!m_open || pageIndex < 0 || pageIndex >= m_pageCount) return false;

    qint64 off = m_file.size();
    m_file.seek(off);
    m_file.write(blob.constData(), blob.size());

    int idx = entryIndex(pageIndex, zoom);
    m_index[idx] = { static_cast<uint64_t>(off), static_cast<uint32_t>(blob.size()), 1, {0,0,0} };

    m_file.seek(entryFileOffset(pageIndex, zoom));
    m_file.write(reinterpret_cast<char*>(&m_index[idx]), sizeof(TileEntry));
    m_file.flush();
    return true;
}

bool TileCacheFile::hasPage(int pageIndex, CacheZoom zoom) const {
    QMutexLocker lock(&m_mutex);
    if (!m_open || pageIndex < 0 || pageIndex >= m_pageCount) return false;
    int idx = entryIndex(pageIndex, zoom);
    return idx < m_index.size() && m_index[idx].status == 1;
}
