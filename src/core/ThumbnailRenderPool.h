#pragma once
#include <QObject>
#include <QImage>
#include <QMutex>
#include <QSet>
#include <QWaitCondition>
#include <QThread>
#include <QAtomicInt>
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

class ThumbnailWorker : public QThread {
    Q_OBJECT
public:
    ThumbnailWorker(FPDF_DOCUMENT doc, int slot, TileCacheFile* cache, quint64 epoch, QObject* parent = nullptr);
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
    std::priority_queue<ThumbRequest, std::vector<ThumbRequest>, std::greater<ThumbRequest>> m_queue;
    QSet<int>      m_queued;   // pages currently in queue — prevents duplicate entries
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

    bool open(const QString& pdfPath);
    void close();
    bool isOpen()    const { return m_open; }
    int  pageCount() const { return m_pageCount; }

    void requestThumbnail(int pageIndex, int priority = 2);
    void prefetchRange(int first, int last);
    const std::vector<ThumbnailWorker*>& workers() const { return m_workers; }
    quint64 epoch() const { return m_epoch; }

signals:
    void thumbnailReady(int pageIndex, QImage image, quint64 epoch);

private:
    std::vector<ThumbnailWorker*> m_workers;
    TileCacheFile m_cache;
    quint64 m_epoch     = 0;
    bool    m_open      = false;
    int     m_pageCount = 0;
    QString m_path;
};
