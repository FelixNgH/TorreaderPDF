#include "PdfLinks.h"
#include "PdfCoords.h"
#include "PageCache.h"
#include <fpdf_doc.h>
#include <fpdf_edit.h>
#include <QtConcurrent>
#include <QElapsedTimer>
#include <QByteArray>

extern QMutex s_pdfiumMutex;

QMutex  PdfLinks::s_mutex;
QHash<quint64, QVector<PdfLink>> PdfLinks::s_linkCache;
QHash<quint64, PdfLinks::PageInfo> PdfLinks::s_infoCache;
QSet<quint64> PdfLinks::s_pending;
quint64 PdfLinks::s_epoch = 0;

// Gia dinh ben goi DA giu s_pdfiumMutex. Doc phai con song. Khong cache.
// Trang muon TU PageCache (SPEC_PAGECACHE_CORE) — khong tu FPDF_LoadPage nua.
static void computePage(FPDF_DOCUMENT doc, int pageIndex,
                        QVector<PdfLink>& links, PdfLinks::PageInfo& info) {
    if (!doc || pageIndex < 0) return;
    QElapsedTimer t; t.start();
    FPDF_PAGE page = PageCache::acquire(doc, pageIndex);
    if (page) {
        info.dispW = FPDF_GetPageWidth(page);
        info.dispH = FPDF_GetPageHeight(page);
        info.rot   = FPDFPage_GetRotation(page) & 3;
        const QPointF box = pdfBoxOrigin(page);
        info.boxX = box.x();
        info.boxY = box.y();

        int startPos = 0;
        FPDF_LINK link = nullptr;
        while (FPDFLink_Enumerate(page, &startPos, &link)) {
            if (!link) continue;
            PdfLink pl;
            FS_RECTF r{};
            if (FPDFLink_GetAnnotRect(link, &r))
                pl.rectPdf = QRectF(QPointF(r.left, r.bottom),
                                    QPointF(r.right, r.top)).normalized();

            FPDF_DEST dest = FPDFLink_GetDest(doc, link);
            FPDF_ACTION action = FPDFLink_GetAction(link);
            if (!dest && action
                && FPDFAction_GetType(action) == PDFACTION_GOTO)
                dest = FPDFAction_GetDest(doc, action);

            if (dest) {
                int pidx = FPDFDest_GetDestPageIndex(doc, dest);
                if (pidx >= 0) {
                    pl.destPage = pidx;
                    pl.destX = -1;
                    pl.destY = -1;
                    FPDF_BOOL hx = 0, hy = 0, hz = 0;
                    FS_FLOAT x = 0, y = 0, z = 0;
                    // /Fit (khong /XYZ): khong co x/y -> destX/destY = -1
                    // de MainWindow roi ve scrollToPage thay vi center (0,0).
                    if (FPDFDest_GetLocationInPage(dest, &hx, &hy, &hz, &x, &y, &z)) {
                        pl.destX = hx ? x : -1;
                        pl.destY = hy ? y : -1;
                    }
                }
            }

            if (action && FPDFAction_GetType(action) == PDFACTION_URI) {
                unsigned long len = FPDFAction_GetURIPath(doc, action, nullptr, 0);
                if (len > 1) {
                    QByteArray buf(static_cast<int>(len), '\0');
                    FPDFAction_GetURIPath(doc, action, buf.data(), len);
                    pl.uri = QString::fromUtf8(buf.left(static_cast<int>(len) - 1));
                }
            }
            // PDFACTION_LAUNCH / REMOTEGOTO / UNSUPPORTED: khong ho tro,
            // van giu trong danh sach de khi bam thi bao "not supported".

            links.append(pl);
        }
        // Trang la cua PageCache — KHONG duoc FPDF_ClosePage o day.
    }
}

QVector<PdfLink> PdfLinks::forPage(FPDF_DOCUMENT doc, int pageIndex) {
    {
        QMutexLocker cacheLk(&s_mutex);
        auto it = s_linkCache.constFind(key(doc, pageIndex));
        if (it != s_linkCache.constEnd()) return *it;
    }

    QVector<PdfLink> links;
    PageInfo info;
    if (doc && pageIndex >= 0) {
        QMutexLocker lk(&s_pdfiumMutex);
        computePage(doc, pageIndex, links, info);
    }

    QMutexLocker cacheLk(&s_mutex);
    s_linkCache.insert(key(doc, pageIndex), links);
    s_infoCache.insert(key(doc, pageIndex), info);
    return links;
}

PdfLinks::CachedPage PdfLinks::cachedForPage(FPDF_DOCUMENT doc, int pageIndex) {
    CachedPage out;
    if (!doc || pageIndex < 0) return out;
    const quint64 k = key(doc, pageIndex);
    QMutexLocker cacheLk(&s_mutex);
    auto it = s_linkCache.constFind(k);
    if (it == s_linkCache.constEnd()) return out;   // chua tinh
    out.ready = true;
    out.links = *it;
    out.info  = s_infoCache.value(k);
    return out;
}

void PdfLinks::requestPage(FPDF_DOCUMENT doc, int pageIndex) {
    if (!doc || pageIndex < 0) return;
    const quint64 k = key(doc, pageIndex);
    quint64 ep = 0;
    {
        QMutexLocker cacheLk(&s_mutex);
        if (s_linkCache.contains(k)) return;   // da tinh xong
        if (s_pending.contains(k)) return;     // dang tinh: khong xep hang lan hai
        s_pending.insert(k);
        ep = s_epoch;
    }

    QtConcurrent::run([doc, pageIndex, k, ep] {
        QVector<PdfLink> links;
        PdfLinks::PageInfo info;
        {
            // Giu s_pdfiumMutex de doc->close() (cung giu mutex nay) khong the
            // giai phong doc trong luc LoadPage.
            QMutexLocker lk(&s_pdfiumMutex);
            {
                QMutexLocker ck(&PdfLinks::s_mutex);
                if (ep != PdfLinks::s_epoch || !PdfLinks::s_pending.contains(k)) {
                    PdfLinks::s_pending.remove(k);
                    return;
                }
            }
            computePage(doc, pageIndex, links, info);
        }
        bool shouldNotify;
        {
            QMutexLocker ck(&PdfLinks::s_mutex);
            shouldNotify = (ep == PdfLinks::s_epoch);
            PdfLinks::s_pending.remove(k);
            if (shouldNotify) {                    // doc con song: ghi dem
                PdfLinks::s_linkCache.insert(k, links);
                PdfLinks::s_infoCache.insert(k, info);
            }
        }
        if (shouldNotify)
            PdfLinks::notifier()->emitReady(doc, pageIndex);
    });
}

PdfLinksNotifier* PdfLinks::notifier() {
    static PdfLinksNotifier n;   // tao lan dau tren GUI thread (view connect)
    return &n;
}

PdfLinks::PageInfo PdfLinks::pageInfo(FPDF_DOCUMENT doc, int pageIndex) {
    {
        QMutexLocker cacheLk(&s_mutex);
        auto it = s_infoCache.constFind(key(doc, pageIndex));
        if (it != s_infoCache.constEnd()) return *it;
    }
    // Chua co thi tinh moi bang forPage (se cache luon).
    forPage(doc, pageIndex);
    QMutexLocker cacheLk(&s_mutex);
    return s_infoCache.value(key(doc, pageIndex));
}

QPointF PdfLinks::dispToPdf(const QPointF& disp, const PageInfo& info) {
    return ::dispToPdf(disp.x(), disp.y(), info.dispW, info.dispH, info.rot,
                       info.boxX, info.boxY);
}

int PdfLinks::linkAt(const QVector<PdfLink>& links, const QPointF& dispPoint,
                     const PageInfo& info) {
    if (links.isEmpty()) return -1;
    const QPointF p = dispToPdf(dispPoint, info);
    for (int i = 0; i < links.size(); ++i) {
        if (links[i].rectPdf.normalized().contains(p)) return i;
    }
    return -1;
}

void PdfLinks::clearCache() {
    QMutexLocker cacheLk(&s_mutex);
    ++s_epoch;              // huy request dang bay (worker kiem tra lai epoch)
    s_pending.clear();
    s_linkCache.clear();
    s_infoCache.clear();
}
