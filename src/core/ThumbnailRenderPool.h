#pragma once
#include <QObject>
#include <QImage>
#include <QMutex>
#include <QHash>
#include <QWaitCondition>
#include <QThread>
#include <QAtomicInt>
#include <atomic>
#include <queue>
#include <vector>
#include <fpdfview.h>
#include "TileCacheFile.h"

struct ThumbRequest {
    int pageIndex;
    int priority; // 0=visible (highest), 1=adjacent, 2=background
    int yieldTries = 0; // ponytail: counter for yield-back attempts per request
    bool operator>(const ThumbRequest& o) const { return priority > o.priority; }
};

class ThumbnailRenderPool;  // Forward declaration (VIỆC: ThumbnailWorker cần pool handle)

class ThumbnailWorker : public QThread {
    Q_OBJECT
public:
    ThumbnailWorker(FPDF_DOCUMENT doc, int slot, TileCacheFile* cache, quint64 epoch,
                    std::atomic<bool>* renderPaused, ThumbnailRenderPool* pool, QObject* parent = nullptr);
    void stop();
    void enqueue(int pageIndex, int priority = 2);

signals:
    void thumbnailReady(int pageIndex, QImage image, quint64 epoch);
protected:
    void run() override;
private:
    FPDF_DOCUMENT m_doc;
    int  m_slot;
    TileCacheFile* m_cache;
    quint64        m_epoch = 0;
    std::atomic<bool>* m_renderPaused = nullptr; // pool's pause flag (may be null for safety)
    ThumbnailRenderPool* m_pool = nullptr;  // VIỆC: để gọi pdfRenderer() lấy pool handle
    std::priority_queue<ThumbRequest, std::vector<ThumbRequest>, std::greater<ThumbRequest>> m_queue;
    QHash<int,int> m_queuedPrio; // page -> best priority pending (0=best); lazy-deletion allows upgrade
    QMutex         m_mutex;
    QWaitCondition m_cond;
    bool           m_stop = false;
};

class ThumbnailRenderPool : public QObject {
    Q_OBJECT
public:
    static constexpr double kThumbScale = 0.15;
    static constexpr int kDocs = 1;

    explicit ThumbnailRenderPool(QObject* parent = nullptr);
    ~ThumbnailRenderPool() override;

    // Mo pool tren CUNG FPDF_DOCUMENT voi renderer chinh (SPEC_PERF_HEAVYPAGE R1): khong
    // tu FPDF_LoadDocument nua de khong parse lai lan thu hai. Pool KHONG dong handle nay —
    // chi tai lieu chu (PdfDocument) moi FPDF_CloseDocument. sharedDoc phai song tu khi
    // open() toi khi close(). Giu nguyen epoch + disk cache. preHash/preSize = hash+size da
    // tinh san (tranh hash lai ca file tren UI thread); 0 = tinh noi bo nhu cu.
    // prePageCount = so trang da biet san (0 = tu hoi PDFium nhu cu). Ly do: hoi PDFium
    // phai xep hang khoa toan cuc, dung hinh giao dien.
    bool open(const QString& pdfPath, FPDF_DOCUMENT sharedDoc,
              uint64_t preHash = 0, uint64_t preSize = 0, int prePageCount = 0);
    void close();
    // R3: nhan thumbnail dung SAN tu ben ngoai (ve bang GPU tu lop vector) — ghi vao
    // cache dia va phat ra nhu mot thumbnail binh thuong, KHONG cham PDFium.
    void insertThumbnail(int pageIndex, const QImage& img);
    bool isOpen()    const { return m_open; }
    int  pageCount() const { return m_pageCount; }

    void requestThumbnail(int pageIndex, int priority = 2);
    void prefetchRange(int first, int last);
    const std::vector<ThumbnailWorker*>& workers() const { return m_workers; }
    quint64 epoch() const { return m_epoch; }

    // Tam dung viec RENDER bang PDFium (doc cache dia van chay binh thuong).
    // Dung khi trang nguoi dung dang xem chua san sang — thumbnail khong duoc
    // gianh s_pdfiumMutex voi no. Mac dinh false.
    void setRenderPaused(bool paused);
    bool isRenderPaused() const;

    // VIỆC THUMBNAIL SONG SONG: truy cập PdfRenderer pool handle.
    // MainWindow gọi sau khi mở tab để cho thumbnail dùng pool handle render không khoá.
    void setPdfRenderer(class PdfRenderer* renderer);
    class PdfRenderer* pdfRenderer() const { return m_pdfRenderer; }

signals:
    void thumbnailReady(int pageIndex, QImage image, quint64 epoch);

private:
    std::vector<ThumbnailWorker*> m_workers;
    TileCacheFile m_cache;
    quint64 m_epoch     = 0;
    bool    m_open      = false;
    int     m_pageCount = 0;
    QString m_path;
    std::atomic<bool> m_renderPaused{false};
    class PdfRenderer* m_pdfRenderer = nullptr;  // VIỆC: pool handle từ renderer
};
