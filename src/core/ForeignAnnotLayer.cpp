#include "ForeignAnnotLayer.h"
#include "OwnAnnotHideGuard.h"
#include "PageCache.h"
#include "PdfiumLock.h"
#include "Bisect.h"
#include <QDebug>
#include <QElapsedTimer>
#include <QMutex>
#include <algorithm>
#include <fpdf_edit.h>
#include <fpdf_annot.h>

extern QMutex s_pdfiumMutex;

// LOC mot annot "ngoai" (khong phai cua app, co noi dung PDFium ve duoc):
//   bo TRUID (markup cua TorReader) · bo HIDDEN · bo POPUP · chi lay co /AP.
// LAI dung cho CA computeForeignBbox LUON build() — mot nguoi cham loc duy nhat.
// GIA DINH dang GIU s_pdfiumMutex. Annot lay ra phai dong bang FPDFPage_CloseAnnot.
bool ForeignAnnotLayer::annotIsForeign(FPDF_ANNOTATION a) {
    if (!a) return false;
    const int flags = FPDFAnnot_GetFlags(a);
    return FPDFAnnot_HasKey(a, "TRUID") == 0
        && !(flags & FPDF_ANNOT_FLAG_HIDDEN)
        && FPDFAnnot_GetSubtype(a) != FPDF_ANNOT_POPUP
        && FPDFAnnot_HasKey(a, "AP");
}

// BBOX hop nhat cac annot NGOAI cua trang: loc qua annotIsForeign. Chi cac annot nay moi
// tao ra pixel khac giua render A/B. Tra false neu khong co annot nao.
// GIA DINH dang GIU s_pdfiumMutex. Annot lay ra phai dong bang FPDFPage_CloseAnnot.
bool ForeignAnnotLayer::computeForeignBbox(FPDF_PAGE page, FS_RECTF& out) {
    bool any = false;
    const int n = FPDFPage_GetAnnotCount(page);
    for (int i = 0; i < n; ++i) {
        FPDF_ANNOTATION a = FPDFPage_GetAnnot(page, i);
        if (!a) continue;
        if (annotIsForeign(a)) {
            FS_RECTF r{};
            if (FPDFAnnot_GetRect(a, &r)) {
                if (!any) { out = r; any = true; }
                else {
                    out.left   = (std::min)(out.left,   r.left);
                    out.top    = (std::max)(out.top,    r.top);
                    out.right  = (std::max)(out.right,  r.right);
                    out.bottom = (std::min)(out.bottom, r.bottom);
                }
            }
        }
        FPDFPage_CloseAnnot(a);
    }
    return any;
}

// Vung bbox (toa do page, goc trai-duoi) -> QRect pixel cung he toa do voi bitmap render
// (goc trai-tren). PDF co truc y huong len; pixel huong xuong: y_px = (pageH - y_pdf)*scale.
QRect ForeignAnnotLayer::bboxToPx(const FS_RECTF& bbox, double pageH, double scale) {
    const int x  = int(bbox.left   * scale);
    const int y  = int((pageH - bbox.top) * scale);
    const int w  = (std::max)(1, int((bbox.right - bbox.left)   * scale));
    const int h  = (std::max)(1, int((bbox.top - bbox.bottom) * scale));
    return QRect(x, y, w, h);
}

// Gao nen trang cua mot o render va blit vao anh chung TRONG SUOT.
// PDFium ve nen trang bang TRANG TINH (255,255,255) — do la "khong co gi" quanh chu thich,
// nen bo no (giu trong suot de nen vector lo ra); giu moi pixel khac trang = muc chu thich
// (ke ca vien khang chai cua chu — do mau nen khac trang, van giu).
// Tra so pixel non-transparent da ghi. dx/dy = goc trai-tren cua patch trong anh chung.
// ponytail: white-key, tran truong hop chu thich MAU TRANG tren nen toi (hiem) —
//           neau seam thi nang len phep tru A/B rieng tung o (van re, ROI da do).
static int blitWhiteKey(QImage& dst, const QImage& src, int dx, int dy) {
    const int dw = dst.width(), dh = dst.height();
    int count = 0;
    for (int y = 0; y < src.height(); ++y) {
        const int ty = dy + y;
        if (ty < 0 || ty >= dh) continue;
        const QRgb* rs = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb*       dr = reinterpret_cast<QRgb*>(dst.scanLine(ty));
        for (int x = 0; x < src.width(); ++x) {
            const int tx = dx + x;
            if (tx < 0 || tx >= dw) continue;
            const QRgb p = rs[x];
            if (qRed(p) >= 250 && qGreen(p) >= 250 && qBlue(p) >= 250) continue;   // nen trang
            dr[tx] = qRgba(qRed(p), qGreen(p), qBlue(p), 255);
            ++count;
        }
    }
    return count;
}

// ── LOP COMMENT — sinh anh TRONG SUOT gom moi chu thich ngoai cua trang ──────────────
// MOT lan render clip dung o TUNG annot (do --annotroi-bench: ~20.9ms cold, <1ms warm),
// gao nen trang, blit vao anh chung. KHONG render ca trang, KHONG phep tru hai lan.
// Khoa PDFium chi GIU trong tung o (<25ms/o) — tha khoa giua cac o de UI xen vao duoc,
// nen lockwait khong the bung len giay nhu ban cu (giu khoa 18-20s/ca trang).
bool ForeignAnnotLayer::build(FPDF_DOCUMENT doc, int pageIndex, int maxPx) {
    m_ready = false;
    m_page  = pageIndex;
    m_img   = QImage();
    // 0927 LƯỢT 10 (--no-fgn): KHÔNG dựng lớp bù — trả false TRƯỚC khi đụng PDFium.
    if (trNoFgn()) return false;
    if (!doc || maxPx <= 0) return false;
    QElapsedTimer tTotal; tTotal.start();

    // PHA 1 — kich thuoc trang + ti le hien thi (GIU khoa, ngan)
    double w = 0, h = 0;
    {
        if (m_cancel.load()) return false;
        TimedPdfiumLock lk(__FILE__, __LINE__);
        FPDF_PAGE page = PageCache::acquire(doc, pageIndex);
        if (!page) return false;
        PageCache::PageBorrow _b(doc, pageIndex);   // RAII — di cap voi acquire
        w = FPDF_GetPageWidth(page);
        h = FPDF_GetPageHeight(page);
    }
    if (m_cancel.load()) return false;
    const double longSide = (std::max)(w, h);
    const double scale = double(maxPx) / (std::max)(longSide, 1.0);
    const int imgW = (std::max)(1, int(w * scale));
    const int imgH = (std::max)(1, int(h * scale));

    m_img = QImage(imgW, imgH, QImage::Format_ARGB32);
    if (m_img.isNull()) {
        qDebug().noquote() << "[commentlayer] ALLOC FAIL page=" << pageIndex
                           << "size=" << imgW << "x" << imgH;
        return false;
    }
    m_img.fill(Qt::transparent);

    const int margin = 4;   // px le quanh o chu thich — khop --annotroi-bench

    // PHA 2 — dem annot (GIU khoa, ngan)
    int n = 0;
    {
        if (m_cancel.load()) return false;
        TimedPdfiumLock lk(__FILE__, __LINE__);
        FPDF_PAGE page = PageCache::acquire(doc, pageIndex);
        if (!page) return false;
        PageCache::PageBorrow _b(doc, pageIndex);
        n = FPDFPage_GetAnnotCount(page);
    }
    if (m_cancel.load()) return false;

    // PHA 3 — render TUNG o, blit vao anh chung. Patch + toa do tinh trong khoa,
    //         blit (khong cham PDFium) ngoai khoa.
    qint64 blittedPx = 0, annotsDrawn = 0;
    int nForeign = 0;
    for (int i = 0; i < n; ++i) {
        if (m_cancel.load()) return false;
        QImage patch;
        int px = 0, py = 0;
        {
            TimedPdfiumLock lk(__FILE__, __LINE__);
            FPDF_PAGE page = PageCache::acquire(doc, pageIndex);
            if (!page) return false;
            PageCache::PageBorrow _b(doc, pageIndex);
            OwnAnnotHideGuard hide(page, true, /*overlayOnly=*/false);
            FPDF_ANNOTATION a = FPDFPage_GetAnnot(page, i);
            if (!a) continue;
            FS_RECTF r{};
            const bool foreign = annotIsForeign(a);
            bool ok = foreign && FPDFAnnot_GetRect(a, &r);
            FPDFPage_CloseAnnot(a);
            if (!foreign) continue;
            ++nForeign;
            const double rw = r.right - r.left, rh = r.top - r.bottom;
            if (!ok || rw <= 0 || rh <= 0) continue;

            // Thiet bi hoa 2 goc cua rect bang DUNG bien cua FPDF_RenderPageBitmap
            // (start=0,0 size=fullW,fullH rotate=rot) -> PDFium tu dung MediaBox origin
            // + rotation. (Gia dinh thu cong goc (0,0) da SAI: tep that co rect y=-794
            //  ngoai [0,pageH] => render ra trang tinh.)
            const int rot   = FPDFPage_GetRotation(page) & 3;
            const int fullW = imgW, fullH = imgH;
            int cx1, cy1, cx2, cy2;
            FPDF_PageToDevice(page, 0, 0, fullW, fullH, rot, r.left,  r.top,    &cx1, &cy1);
            FPDF_PageToDevice(page, 0, 0, fullW, fullH, rot, r.right, r.bottom, &cx2, &cy2);
            const int ax = (std::min)(cx1, cx2), ay = (std::min)(cy1, cy2);
            const int aw = qMax(1, qAbs(cx2 - cx1)), ah = qMax(1, qAbs(cy2 - cy1));
            const int bw = aw + 2 * margin, bh = ah + 2 * margin;
            const int ox = margin - ax, oy = margin - ay;   // lech de o annot roi vao (margin,margin)
            patch = QImage(bw, bh, QImage::Format_ARGB32);
            if (patch.isNull()) return false;
            patch.fill(Qt::white);   // PDFium se ve de len nen trang + chu thich
            FPDF_BITMAP bmp = FPDFBitmap_CreateEx(bw, bh, FPDFBitmap_BGRA,
                                                  patch.bits(), patch.bytesPerLine());
            if (bmp) {
                FPDF_RenderPageBitmap(bmp, page, ox, oy, fullW, fullH, rot,
                                      FPDF_ANNOT | FPDF_RENDER_LIMITEDIMAGECACHE);
                FPDFBitmap_Destroy(bmp);
            }
            px = ax - margin;   // patch (margin,margin) = goc trai-tren annot trong anh chung
            py = ay - margin;
        }
        const int c = blitWhiteKey(m_img, patch, px, py);
        if (c > 0) ++annotsDrawn;
        blittedPx += c;
    }

    m_ready = (blittedPx > 0);
    const qint64 msTotal = tTotal.elapsed();
    qDebug().noquote() << "[commentlayer] page=" << pageIndex
                       << "size=" << imgW << "x" << imgH
                       << "annots=" << n << "foreign=" << nForeign << "drawn=" << annotsDrawn
                       << "px=" << blittedPx << "ready=" << m_ready << "ms=" << msTotal;

    // CHOT AN TOAN — build qua lau thi HUY, tha khong co lop comment con hon treo app.
    // (Khoa chi giu tung o nen day la tran TONG thoi gian nen, khong phai tran lockwait.)
    if (msTotal > 3000) {
        m_cancel = true;
        m_ready  = false;
        qDebug().noquote() << "[commentlayer] ABORT qua lau page=" << pageIndex << "ms=" << msTotal;
        return false;
    }
    return m_ready;
}

bool ForeignAnnotLayer::buildRegion(FPDF_DOCUMENT doc, int pageIndex,
                                    double scale, QRect regionPx) {
    m_regReady = false;
    m_regPage  = pageIndex;
    m_regScale = scale;
    m_regRect  = regionPx;
    m_regImg   = QImage();
    // 0927 LƯỢT 10 (--no-fgn): KHÔNG dựng vùng bù — trả false TRƯỚC khi đụng PDFium.
    if (trNoFgn()) return false;
    if (!doc || scale <= 0.0 || regionPx.width() <= 0 || regionPx.height() <= 0) return false;

    const qint64 px = qint64(regionPx.width()) * qint64(regionPx.height());
    if (px > 40000000LL) {
        qDebug().noquote() << "[fgnlayer] region BO QUA — qua lon px=" << px;
        return false;
    }

    // PHA 1 — lay kich thuoc trang (GIU khoa)
    QElapsedTimer tSize; tSize.start();
    double w = 0, h = 0;
    {
        if (m_cancel.load()) return false;
        TimedPdfiumLock lk(__FILE__, __LINE__);
        FPDF_PAGE page = PageCache::acquire(doc, pageIndex);
        if (!page) return false;
        PageCache::PageBorrow _b(doc, pageIndex);   // RAII — di cap voi acquire
        w = FPDF_GetPageWidth(page);
        h = FPDF_GetPageHeight(page);
    }
    const qint64 msSize = tSize.elapsed();
    if (m_cancel.load()) return false;
    const int fullW = (std::max)(1, int(w * scale));
    const int fullH = (std::max)(1, int(h * scale));
    const int rw = regionPx.width();
    const int rh = regionPx.height();

    const int flagsBase = FPDF_RENDER_LIMITEDIMAGECACHE;

    // PHA 2 — render B (KHONG annot) vao imgB (GIU khoa, rieng mot lan)
    QElapsedTimer tB; tB.start();
    QImage imgB(rw, rh, QImage::Format_ARGB32);
    if (imgB.isNull()) { qDebug().noquote() << "[fgnlayer] ALLOC FAIL page=" << pageIndex
                                            << "size=" << rw << "x" << rh; return false; }
    imgB.fill(Qt::white);
    {
        if (m_cancel.load()) return false;
        TimedPdfiumLock lk(__FILE__, __LINE__);
        FPDF_PAGE page = PageCache::acquire(doc, pageIndex);
        if (!page) return false;
        PageCache::PageBorrow _b(doc, pageIndex);
        FPDF_BITMAP bmp = FPDFBitmap_CreateEx(rw, rh, FPDFBitmap_BGRA,
                                              imgB.bits(), imgB.bytesPerLine());
        if (!bmp) return false;
        FPDF_RenderPageBitmap(bmp, page, -regionPx.x(), -regionPx.y(),
                              fullW, fullH, 0, flagsBase);
        FPDFBitmap_Destroy(bmp);
    }
    const qint64 msB = tB.elapsed();
    if (m_cancel.load()) return false;

    // PHA 3 — render A (CO annot, an annot cua chinh TorReader). Guard PHAI nam trong khoa.
    QElapsedTimer tA; tA.start();
    QImage imgA(rw, rh, QImage::Format_ARGB32);
    if (imgA.isNull()) { qDebug().noquote() << "[fgnlayer] ALLOC FAIL page=" << pageIndex
                                            << "size=" << rw << "x" << rh; return false; }
    imgA.fill(Qt::white);
    {
        if (m_cancel.load()) return false;
        TimedPdfiumLock lk(__FILE__, __LINE__);
        FPDF_PAGE page = PageCache::acquire(doc, pageIndex);
        if (!page) return false;
        PageCache::PageBorrow _b(doc, pageIndex);
        OwnAnnotHideGuard hide(page, true, /*overlayOnly=*/true);
        FPDF_BITMAP bmp = FPDFBitmap_CreateEx(rw, rh, FPDFBitmap_BGRA,
                                              imgA.bits(), imgA.bytesPerLine());
        if (!bmp) return false;
        FPDF_RenderPageBitmap(bmp, page, -regionPx.x(), -regionPx.y(),
                              fullW, fullH, 0, flagsBase | FPDF_ANNOT);
        FPDFBitmap_Destroy(bmp);
    }
    const qint64 msA = tA.elapsed();
    if (m_cancel.load()) return false;

    // PHA 4 — tru pixel (KHONG khoa)
    QElapsedTimer tDiff; tDiff.start();
    if (m_cancel.load()) return false;
    m_regImg = QImage(rw, rh, QImage::Format_ARGB32);
    if (m_regImg.isNull()) { qDebug().noquote() << "[fgnlayer] ALLOC FAIL page=" << pageIndex
                                                << "size=" << rw << "x" << rh; return false; }
    m_regImg.fill(Qt::transparent);
    qint64 diffPx = 0;
    for (int y = 0; y < rh; ++y) {
        if (m_cancel.load()) return false;
        const QRgb* ra = reinterpret_cast<const QRgb*>(imgA.constScanLine(y));
        const QRgb* rb = reinterpret_cast<const QRgb*>(imgB.constScanLine(y));
        QRgb*       ro = reinterpret_cast<QRgb*>(m_regImg.scanLine(y));
        for (int x = 0; x < rw; ++x) {
            if ((ra[x] & 0x00FFFFFF) == (rb[x] & 0x00FFFFFF)) continue;
            ro[x] = qRgba(qRed(ra[x]), qGreen(ra[x]), qBlue(ra[x]), 255);
            ++diffPx;
        }
    }
    const qint64 msDiff = tDiff.elapsed();

    m_regReady = true;
    qDebug().noquote() << "[fgnlayer] REGION page=" << pageIndex
                       << "rect=" << regionPx << "scale=" << scale
                       << "diffPx=" << diffPx << "ms=" << (msSize + msB + msA + msDiff)
                       << "phases sizeMs=" << msSize << "renderBMs=" << msB
                       << "renderAMs=" << msA << "diffMs=" << msDiff;
    return true;
}
