#pragma once
#include <QString>
#include <QImage>
#include <QVector>
#include <QFile>
#include <QMutex>
#include <cstdint>

enum class CacheZoom : uint8_t { Thumb=0, Full=1, Double=2, Quad=3 };

CacheZoom zoomBandFor(double scaleFactor);
double canonicalScale(CacheZoom band);

#pragma pack(push, 1)
struct TorCacheHeader {
    char     magic[8];       // "TORCACH2"
    uint64_t pdfHash;
    uint64_t pdfSize;
    uint32_t pageCount;
    uint8_t  zoomLevels;     // =4
    uint8_t  jpegQuality;    // =85
    uint8_t  reserved[34];
};
static_assert(sizeof(TorCacheHeader) == 64, "TorCacheHeader must be 64 bytes");

struct TileEntry {
    uint64_t offset;   // 0 = not rendered
    uint32_t size;
    uint8_t  status;   // 0=empty 1=ready 2=error
    uint8_t  pad[3];
};
static_assert(sizeof(TileEntry) == 16, "TileEntry must be 16 bytes");
#pragma pack(pop)

class TileCacheFile {
public:
    static uint64_t hashFile(const QString& pdfPath);

    bool open(const QString& pdfPath, uint64_t pdfHash, uint64_t pdfSize, int pageCount);
    void close();
    bool isOpen() const { return m_open; }

    QImage readPage(int pageIndex, CacheZoom zoom);
    bool writePage(int pageIndex, CacheZoom zoom, const QImage& image);
    bool hasPage(int pageIndex, CacheZoom zoom) const;

private:
    mutable QMutex     m_mutex;
    int entryIndex(int page, CacheZoom z) const { return page*4 + (int)z; }
    qint64 entryFileOffset(int page, CacheZoom z) const {
        return (qint64)sizeof(TorCacheHeader) + entryIndex(page,z)*(qint64)sizeof(TileEntry);
    }

    QString            m_path;
    QFile              m_file;
    int                m_pageCount = 0;
    TorCacheHeader     m_header{};
    QVector<TileEntry> m_index;
    bool               m_open = false;
    int                m_jpegQuality = 85;
};
