#include "ForeignAnnotLayer.h"
#include "OwnAnnotHideGuard.h"
#include "PageCache.h"
#include "PdfiumLock.h"
#include <QDebug>
#include <QElapsedTimer>
#include <QMutex>
#include <algorithm>
#include <fpdf_edit.h>
#include <fpdf_annot.h>

extern QMutex s_pdfiumMutex;

// BBOX hop nhat cac annot NGOAI cua trang: khong TRUID (cua TorReader), khong HIDDEN,
// khong POPUP, co /AP. Chi cac annot nay moi tao ra pixel khac giua render A/B. Tra false
// neu khong co annot nao (=> phep tru hai lan render se bang 0, khong can ve ca trang).
// GIA DINH dang GIU s_pdfiumMutex. Annot lay ra phai dong bang FPDFPage_CloseAnnot.
bool ForeignAnnotLayer::computeForeignBbox(FPDF_PAGE page, FS_RECTF& out) {
    bool any = false;
    const int n = FPDFPage_GetAnnotCount(page);
    for (int i = 0; i < n; ++i) {
        FPDF_ANNOTATION a = FPDFPage_GetAnnot(page, i);
        if (!a) continue;
        const int flags = FPDFAnnot_GetFlags(a);
        const bool isForeign = FPDFAnnot_HasKey(a, "TRUID") == 0
                            && !(flags & FPDF_ANNOT_FLAG_HIDDEN)
                            && FPDFAnnot_GetSubtype(a) != FPDF_ANNOT_POPUP
                            && FPDFAnnot_HasKey(a, "AP");
        if (isForeign) {
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

bool ForeignAnnotLayer::build(FPDF_DOCUMENT doc, int pageIndex, int maxPx) {
    m_ready = false;
    m_page  = pageIndex;
    m_img   = QImage();
    if (!doc) return false;
    QElapsedTimer tTotal; tTotal.start();

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
    const double longSide = (std::max)(w, h);
    const double scale = double(maxPx) / (std::max)(longSide, 1.0);
    const int imgW = (std::max)(1, int(w * scale));
    const int imgH = (std::max)(1, int(h * scale));

    const int flagsBase = FPDF_RENDER_LIMITEDIMAGECACHE;

    // PHA 1.5 — BBOX hop nhat cac annot NGOAI (GIU khoa). Bbox rong thi roi vao fullpage
    //     (duong cu) de khong bo sot annot khong /AP ma pdfium van ve.
    QRect bboxPx;
    bool useRegion = false;
    {
        if (m_cancel.load()) return false;
        TimedPdfiumLock lk(__FILE__, __LINE__);
        FPDF_PAGE page = PageCache::acquire(doc, pageIndex);
        if (!page) return false;
        PageCache::PageBorrow _b(doc, pageIndex);
        FS_RECTF bbox{};
        if (computeForeignBbox(page, bbox)) {
            bboxPx = bboxToPx(bbox, h, scale).intersected(QRect(0, 0, imgW, imgH));
        }
    }
    if (m_cancel.load()) return false;

    // BBOX rong (khong co annot ngoai co /AP): con nhieu annot khong /AP ma pdfium van ve
    // (vi du FreeText sinh appearance noi bo) => de an toan, roi vao fullpage de giu duong
    // cu. useRegion chi bat khi co bbox < 60% dien tich.
    const double pageAreaPx = double(imgW) * double(imgH);
    const double bboxAreaPx = bboxPx.isEmpty()
                                ? 0.0
                                : double(bboxPx.width()) * double(bboxPx.height());
    const double pct = pageAreaPx > 0.0 ? 100.0 * bboxAreaPx / pageAreaPx : 100.0;
    useRegion = !bboxPx.isEmpty() && pct < 60.0;
    qDebug().noquote() << "[fgnlayer] bbox page=" << pageIndex
                       << "pct=" << pct << "mode=" << (useRegion ? "region" : "fullpage");

    qint64 msB = 0, msA = 0, msDiff = 0;
    qint64 diffPx = 0;

    if (useRegion) {
        // ── REGION MODE — chi render VUNG BBOX (tai dung ma cua buildRegion) ──
        const int rw = bboxPx.width();
        const int rh = bboxPx.height();

        // PHA 2 — render B (KHONG annot) trong vung bbox
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
            FPDF_RenderPageBitmap(bmp, page, -bboxPx.x(), -bboxPx.y(),
                                  imgW, imgH, 0, flagsBase);
            FPDFBitmap_Destroy(bmp);
        }
        msB = tB.elapsed();
        if (m_cancel.load()) return false;

        // PHA 3 — render A (CO annot, AN annot cua TorReader) trong vung bbox.
        //     Guard PHAI nam trong khoa (no sua co annot cua trang).
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
            // 🔴🔴 SUA TAN GOC 2026-09-01: tham so thu 3 (overlayOnly) truoc day de MAC DINH = false,
            // tuc GIAU MOI annot co TRUID — nghia la lop bu chi chua chu thich NGOAI.
            // Hau qua: tep chi co Note/Text CUA APP thi hai luot ve giong het nhau ⇒ diffPx = 0
            // ⇒ lop bu tu danh dau "chua san sang" ⇒ khong bao gio dung duoc ⇒ trang co Note/Text
            //   buoc phai bo nen vector (mat net) moi thay chu thich.
            // Dung phai la: chi giau nhung gi OVERLAY THUC SU VE LAI. Nhung gi overlay khong ve
            // (Note, Text, chu thich ngoai) phai NAM TRONG lop bu, de dat len nen vector.
            OwnAnnotHideGuard hide(page, true, /*overlayOnly=*/true);
            FPDF_BITMAP bmp = FPDFBitmap_CreateEx(rw, rh, FPDFBitmap_BGRA,
                                                  imgA.bits(), imgA.bytesPerLine());
            if (!bmp) return false;
            FPDF_RenderPageBitmap(bmp, page, -bboxPx.x(), -bboxPx.y(),
                                  imgW, imgH, 0, flagsBase | FPDF_ANNOT);
            FPDFBitmap_Destroy(bmp);
        }
        msA = tA.elapsed();
        if (m_cancel.load()) return false;

        // PHA 4 — tru pixel trong vung bbox, ghep vao m_img (toan trang, trong suot ngoai vung)
        QElapsedTimer tDiff; tDiff.start();
        m_img = QImage(imgW, imgH, QImage::Format_ARGB32);
        if (m_img.isNull()) { qDebug().noquote() << "[fgnlayer] ALLOC FAIL page=" << pageIndex
                                                 << "size=" << imgW << "x" << imgH; return false; }
        m_img.fill(Qt::transparent);
        for (int y = 0; y < rh; ++y) {
            if (m_cancel.load()) return false;
            const QRgb* ra = reinterpret_cast<const QRgb*>(imgA.constScanLine(y));
            const QRgb* rb = reinterpret_cast<const QRgb*>(imgB.constScanLine(y));
            QRgb*       ro = reinterpret_cast<QRgb*>(m_img.scanLine(y + bboxPx.y()));
            for (int x = 0; x < rw; ++x) {
                if ((ra[x] & 0x00FFFFFF) == (rb[x] & 0x00FFFFFF)) continue;
                ro[x + bboxPx.x()] = qRgba(qRed(ra[x]), qGreen(ra[x]), qBlue(ra[x]), 255);
                ++diffPx;
            }
        }
        msDiff = tDiff.elapsed();
        m_ready = (diffPx > 0);
    } else {
        // ── FULLPAGE MODE — bbox >= 60% dien tich, giu nguyen duong cu ──
        // PHA 2 — render B (KHONG annot) vao imgB (GIU khoa, rieng mot lan)
        QElapsedTimer tB; tB.start();
        QImage imgB(imgW, imgH, QImage::Format_ARGB32);
        if (imgB.isNull()) { qDebug().noquote() << "[fgnlayer] ALLOC FAIL page=" << pageIndex
                                                << "size=" << imgW << "x" << imgH; return false; }
        imgB.fill(Qt::white);
        {
            if (m_cancel.load()) return false;
            TimedPdfiumLock lk(__FILE__, __LINE__);
            FPDF_PAGE page = PageCache::acquire(doc, pageIndex);
            if (!page) return false;
            PageCache::PageBorrow _b(doc, pageIndex);
            FPDF_BITMAP bmp = FPDFBitmap_CreateEx(imgW, imgH, FPDFBitmap_BGRA,
                                                  imgB.bits(), imgB.bytesPerLine());
            if (!bmp) return false;
            FPDF_RenderPageBitmap(bmp, page, 0, 0, imgW, imgH, 0, flagsBase);
            FPDFBitmap_Destroy(bmp);
        }
        msB = tB.elapsed();
        if (m_cancel.load()) return false;

        // PHA 3 — render A (CO annot, nhung AN annot cua CHINH TorReader). Guard PHAI nam
        //     trong khoa: no sua co annot cua trang; tha khoa giua chung se lam luong khac
        //     render nham trang thai.
        QElapsedTimer tA; tA.start();
        QImage imgA(imgW, imgH, QImage::Format_ARGB32);
        if (imgA.isNull()) { qDebug().noquote() << "[fgnlayer] ALLOC FAIL page=" << pageIndex
                                                << "size=" << imgW << "x" << imgH; return false; }
        imgA.fill(Qt::white);
        {
            if (m_cancel.load()) return false;
            TimedPdfiumLock lk(__FILE__, __LINE__);
            FPDF_PAGE page = PageCache::acquire(doc, pageIndex);
            if (!page) return false;
            PageCache::PageBorrow _b(doc, pageIndex);
            // 🔴🔴 SUA TAN GOC 2026-09-01: tham so thu 3 (overlayOnly) truoc day de MAC DINH = false,
            // tuc GIAU MOI annot co TRUID — nghia la lop bu chi chua chu thich NGOAI.
            // Hau qua: tep chi co Note/Text CUA APP thi hai luot ve giong het nhau ⇒ diffPx = 0
            // ⇒ lop bu tu danh dau "chua san sang" ⇒ khong bao gio dung duoc ⇒ trang co Note/Text
            //   buoc phai bo nen vector (mat net) moi thay chu thich.
            // Dung phai la: chi giau nhung gi OVERLAY THUC SU VE LAI. Nhung gi overlay khong ve
            // (Note, Text, chu thich ngoai) phai NAM TRONG lop bu, de dat len nen vector.
            OwnAnnotHideGuard hide(page, true, /*overlayOnly=*/true);
            FPDF_BITMAP bmp = FPDFBitmap_CreateEx(imgW, imgH, FPDFBitmap_BGRA,
                                                  imgA.bits(), imgA.bytesPerLine());
            if (!bmp) return false;
            FPDF_RenderPageBitmap(bmp, page, 0, 0, imgW, imgH, 0, flagsBase | FPDF_ANNOT);
            FPDFBitmap_Destroy(bmp);
        }
        msA = tA.elapsed();
        if (m_cancel.load()) return false;

        // PHA 4 — tru pixel (KHONG khoa). PDFium tat dinh nen cho nao noi dung khong doi thi
        // 2 anh giong HET => so bang nhau tuyet doi, khong can nguong.
        QElapsedTimer tDiff; tDiff.start();
        m_img = QImage(imgW, imgH, QImage::Format_ARGB32);
        if (m_img.isNull()) { qDebug().noquote() << "[fgnlayer] ALLOC FAIL page=" << pageIndex
                                                 << "size=" << imgW << "x" << imgH; return false; }
        m_img.fill(Qt::transparent);
        for (int y = 0; y < imgH; ++y) {
            if (m_cancel.load()) return false;
            const QRgb* ra = reinterpret_cast<const QRgb*>(imgA.constScanLine(y));
            const QRgb* rb = reinterpret_cast<const QRgb*>(imgB.constScanLine(y));
            QRgb*       ro = reinterpret_cast<QRgb*>(m_img.scanLine(y));
            for (int x = 0; x < imgW; ++x) {
                if ((ra[x] & 0x00FFFFFF) == (rb[x] & 0x00FFFFFF)) continue;
                ro[x] = qRgba(qRed(ra[x]), qGreen(ra[x]), qBlue(ra[x]), 255);
                ++diffPx;
            }
        }
        msDiff = tDiff.elapsed();
        m_ready = (diffPx > 0);
    }

    const qint64 msTotal = tTotal.elapsed();
    qDebug().noquote() << "[fgnlayer] phases page=" << pageIndex
                       << "sizeMs=" << msSize << "renderBMs=" << msB
                       << "renderAMs=" << msA << "diffMs=" << msDiff;
    qDebug().noquote() << "[fgnlayer] page=" << pageIndex
                       << "size=" << imgW << "x" << imgH
                       << "diffPx=" << diffPx << "ready=" << m_ready
                       << "ms=" << msTotal;

    // CHOT AN TOAN — tong thoi gian build qua 3000 ms thi HUY (dung m_cancel da co) va
    // tra false. Tha khong co lop bu con hon treo app.
    if (msTotal > 3000) {
        m_cancel = true;
        qDebug().noquote() << "[fgnlayer] ABORT qua lau page=" << pageIndex << "ms=" << msTotal;
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
        // 🔴🔴 SUA TAN GOC 2026-09-01: tham so thu 3 (overlayOnly) truoc day de MAC DINH = false,
            // tuc GIAU MOI annot co TRUID — nghia la lop bu chi chua chu thich NGOAI.
            // Hau qua: tep chi co Note/Text CUA APP thi hai luot ve giong het nhau ⇒ diffPx = 0
            // ⇒ lop bu tu danh dau "chua san sang" ⇒ khong bao gio dung duoc ⇒ trang co Note/Text
            //   buoc phai bo nen vector (mat net) moi thay chu thich.
            // Dung phai la: chi giau nhung gi OVERLAY THUC SU VE LAI. Nhung gi overlay khong ve
            // (Note, Text, chu thich ngoai) phai NAM TRONG lop bu, de dat len nen vector.
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
