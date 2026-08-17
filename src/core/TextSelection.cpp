#include "TextSelection.h"
#include "PdfCoords.h"
#include "PageCache.h"
#include <QMutex>
#include <QList>
#include <QChar>
#include <fpdf_edit.h>
#include <vector>

extern QMutex s_pdfiumMutex;

namespace TextSelection {

// ── Page + text page MUON TU PageCache — NGUON SU THAT DUY NHAT (SPEC_PAGECACHE_CORE) ──
// Khong con cache rieng o day: PageCache giu FPDF_PAGE (LRU 6) + FPDF_TEXTPAGE
// theo trang. Ben goi chi muon trong luc dung (giu s_pdfiumMutex khi truy xuat).

PageInfo pageFor(FPDF_DOCUMENT doc, int page) {
    PageInfo out;
    if (!doc || page < 0) return out;
    QMutexLocker lk(&s_pdfiumMutex);
    FPDF_PAGE pg = PageCache::acquire(doc, page);
    if (!pg) return out;
    FPDF_TEXTPAGE tp = PageCache::textPage(doc, page);
    if (!tp) return out;
    PageCache::PageMeta meta;
    if (!PageCache::metaFor(doc, page, meta)) return out;
    out.page = pg; out.tp = tp;
    out.rot = meta.rot; out.box = meta.box; out.disp = meta.disp;
    return out;
}

PageInfo pageForCached(FPDF_DOCUMENT doc, int page) {
    PageInfo out;
    if (!doc || page < 0) return out;
    FPDF_PAGE pg = PageCache::tryAcquire(doc, page);
    if (!pg) return out;
    FPDF_TEXTPAGE tp = PageCache::tryAcquireTextPage(doc, page);
    if (!tp) return out;
    PageCache::PageMeta meta;
    if (!PageCache::metaFor(doc, page, meta)) return out;
    out.page = pg; out.tp = tp;
    out.rot = meta.rot; out.box = meta.box; out.disp = meta.disp;
    return out;
}

void closePage(FPDF_DOCUMENT doc, int page) {
    if (!doc || page < 0) return;
    QMutexLocker lk(&s_pdfiumMutex);
    PageCache::invalidate(doc, page);
}

void closeDocument(FPDF_DOCUMENT doc) {
    if (!doc) return;
    QMutexLocker lk(&s_pdfiumMutex);
    PageCache::forgetDocument(doc);
}

QPointF dispToPagePt(const PageInfo& info, const QPointF& disp) {
    return dispToPdf(disp.x(), disp.y(),
                     info.disp.width(), info.disp.height(),
                     info.rot, info.box.x(), info.box.y());
}

QRectF pageRectToDispPt(const PageInfo& info, const QRectF& r) {
    return pdfRectToDisp(r, info.disp.width(), info.disp.height(),
                         info.rot, info.box.x(), info.box.y());
}

int charIndexAt(FPDF_TEXTPAGE tp, double x, double y, double tolX, double tolY) {
    if (!tp) return -1;
    QMutexLocker lock(&s_pdfiumMutex);
    return FPDFText_GetCharIndexAtPos(tp, x, y, tolX, tolY);
}

int nearestCharAt(FPDF_TEXTPAGE tp, double x, double y, double tolY) {
    if (!tp) return -1;
    QMutexLocker lock(&s_pdfiumMutex);
    const int total = FPDFText_CountChars(tp);
    if (total <= 0) return -1;
    // Tim DONG (line rect) chua y (dung CountRects+GetRect — cung nguon su
    // that voi highlight search, khong tu gop).
    const int nLines = FPDFText_CountRects(tp, 0, total);
    int lineIdx = -1;
    for (int i = 0; i < nLines; ++i) {
        double l = 0, t = 0, r = 0, b = 0;
        if (!FPDFText_GetRect(tp, i, &l, &t, &r, &b)) continue;
        if (y >= b - tolY && y <= t + tolY) { lineIdx = i; break; }
    }
    if (lineIdx < 0) return -1;
    double ll = 0, lt = 0, lr = 0, lb = 0;
    if (!FPDFText_GetRect(tp, lineIdx, &ll, &lt, &lr, &lb)) return -1;
    // Ky tu co tam ngang gan x nhat, co tam doc nam trong dong.
    int best = -1;
    double bestDx = 1e18;
    for (int i = 0; i < total; ++i) {
        double cl = 0, cb = 0, cr = 0, ct = 0;
        if (!FPDFText_GetCharBox(tp, i, &cl, &cr, &cb, &ct)) continue;
        const double cx = (cl + cr) / 2.0;
        const double cy = (cb + ct) / 2.0;
        if (cy < lb - tolY || cy > lt + tolY) continue;
        const double dx = qAbs(cx - x);
        if (dx < bestDx) { bestDx = dx; best = i; }
    }
    return best;
}

QVector<QRectF> rectsForRange(FPDF_TEXTPAGE tp, int start, int count) {
    QVector<QRectF> out;
    if (!tp || count <= 0) return out;
    QMutexLocker lock(&s_pdfiumMutex);
    const int n = FPDFText_CountRects(tp, start, count);
    for (int i = 0; i < n; ++i) {
        double l = 0, t = 0, r = 0, b = 0;
        if (!FPDFText_GetRect(tp, i, &l, &t, &r, &b)) continue;
        out.append(QRectF(l, b, r - l, t - b));   // Y-up: y=bottom, h=top-bottom
    }
    return out;
}

QVector<QRectF> rectsForRangeDisp(const PageInfo& info, int start, int count) {
    const QVector<QRectF> pageSpace = rectsForRange(info.tp, start, count);
    QVector<QRectF> disp;
    disp.reserve(pageSpace.size());
    for (const QRectF& r : pageSpace)
        disp.append(pageRectToDispPt(info, r));
    return disp;
}

QString textForRange(FPDF_TEXTPAGE tp, int start, int count) {
    if (!tp || count <= 0) return QString();
    QMutexLocker lock(&s_pdfiumMutex);
    std::vector<unsigned short> buf(static_cast<size_t>(count + 1), 0);
    if (FPDFText_GetText(tp, start, count, buf.data()) < 0) return QString();
    return QString::fromUtf16(buf.data());
}

void wordRange(FPDF_TEXTPAGE tp, int idx, int* start, int* count) {
    *start = 0;
    *count = 0;
    if (!tp || idx < 0) return;
    QMutexLocker lock(&s_pdfiumMutex);
    const int total = FPDFText_CountChars(tp);
    if (idx >= total) return;
    auto isSpace = [&](int i) -> bool {
        if (i < 0 || i >= total) return true;
        unsigned short buf[2] = {0, 0};
        if (FPDFText_GetText(tp, i, 1, buf) < 0) return true;
        return QChar(buf[0]).isSpace();
    };
    int s = idx;
    while (s > 0 && !isSpace(s - 1)) --s;
    int e = idx;
    while (e + 1 < total && !isSpace(e + 1)) ++e;
    *start = s;
    *count = e - s + 1;
}

void lineRange(FPDF_TEXTPAGE tp, int idx, int* start, int* count) {
    *start = 0;
    *count = 0;
    if (!tp || idx < 0) return;
    QMutexLocker lock(&s_pdfiumMutex);
    const int total = FPDFText_CountChars(tp);
    if (idx >= total) return;
    double l = 0, b = 0, r = 0, t = 0;
    if (!FPDFText_GetCharBox(tp, idx, &l, &r, &b, &t)) { *start = idx; *count = 1; return; }
    const double cy  = (b + t) / 2.0;
    const double tol = qMax(1.0, (t - b) * 0.6);   // cung dong = lech doc nho hon 60% chieu cao chu
    int s = idx;
    while (s > 0) {
        double l2 = 0, b2 = 0, r2 = 0, t2 = 0;
        if (!FPDFText_GetCharBox(tp, s - 1, &l2, &r2, &b2, &t2)) break;
        if (qAbs((b2 + t2) / 2.0 - cy) > tol) break;
        --s;
    }
    int e = idx;
    while (e + 1 < total) {
        double l2 = 0, b2 = 0, r2 = 0, t2 = 0;
        if (!FPDFText_GetCharBox(tp, e + 1, &l2, &r2, &b2, &t2)) break;
        if (qAbs((b2 + t2) / 2.0 - cy) > tol) break;
        ++e;
    }
    *start = s;
    *count = e - s + 1;
}

QHash<int, QVector<QRectF>> rangeRectsByPageDisp(FPDF_DOCUMENT doc,
                                                 int anchorPage, int anchorChar,
                                                 int focusPage, int focusChar) {
    QHash<int, QVector<QRectF>> byPage;
    if (!doc || anchorPage < 0 || focusPage < 0 || anchorPage > focusPage) return byPage;
    for (int p = anchorPage; p <= focusPage; ++p) {
        const PageInfo info = pageFor(doc, p);
        if (!info.tp) continue;
        int total = 0;
        { QMutexLocker lock(&s_pdfiumMutex); total = FPDFText_CountChars(info.tp); }
        const int s = (p == anchorPage) ? anchorChar : 0;
        const int e = (p == focusPage) ? focusChar : total - 1;
        if (s < 0 || e < s || e >= total) continue;
        byPage.insert(p, rectsForRangeDisp(info, s, e - s + 1));
    }
    return byPage;
}

QString rangeText(FPDF_DOCUMENT doc,
                  int anchorPage, int anchorChar,
                  int focusPage, int focusChar) {
    if (!doc || anchorPage < 0 || focusPage < 0 || anchorPage > focusPage) return QString();
    QString out;
    for (int p = anchorPage; p <= focusPage; ++p) {
        const PageInfo info = pageFor(doc, p);
        if (!info.tp) continue;
        int total = 0;
        { QMutexLocker lock(&s_pdfiumMutex); total = FPDFText_CountChars(info.tp); }
        const int s = (p == anchorPage) ? anchorChar : 0;
        const int e = (p == focusPage) ? focusChar : total - 1;
        if (s < 0 || e < s || e >= total) continue;
        const QString t = textForRange(info.tp, s, e - s + 1);
        if (!out.isEmpty()) out += QLatin1Char('\n');
        out += t;
    }
    return out;
}

}  // namespace TextSelection
