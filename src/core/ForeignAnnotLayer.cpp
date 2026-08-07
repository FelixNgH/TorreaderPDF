#include "ForeignAnnotLayer.h"
#include "OwnAnnotHideGuard.h"
#include <QDebug>
#include <QElapsedTimer>
#include <algorithm>
#include <fpdf_edit.h>

bool ForeignAnnotLayer::build(FPDF_DOCUMENT doc, int pageIndex, int maxPx) {
    m_ready = false;
    m_page  = pageIndex;
    m_img   = QImage();
    if (!doc) return false;

    QElapsedTimer t; t.start();

    FPDF_PAGE page = FPDF_LoadPage(doc, pageIndex);
    if (!page) return false;

    const double w = FPDF_GetPageWidth(page);
    const double h = FPDF_GetPageHeight(page);
    const double longSide = (std::max)(w, h);
    const double scale = double(maxPx) / (std::max)(longSide, 1.0);
    const int imgW = (std::max)(1, int(w * scale));
    const int imgH = (std::max)(1, int(h * scale));

    const int flagsBase = FPDF_RENDER_LIMITEDIMAGECACHE;

    // B — KHONG annot
    QImage imgB(imgW, imgH, QImage::Format_ARGB32);
    imgB.fill(Qt::white);
    {
        FPDF_BITMAP bmp = FPDFBitmap_CreateEx(imgW, imgH, FPDFBitmap_BGRA,
                                              imgB.bits(), imgB.bytesPerLine());
        if (!bmp) { FPDF_ClosePage(page); return false; }
        FPDF_RenderPageBitmap(bmp, page, 0, 0, imgW, imgH, 0, flagsBase);
        FPDFBitmap_Destroy(bmp);
    }

    // A — CO annot, nhung AN annot cua CHINH TorReader (lop QPainter da ve chung roi,
    //     de nguyen se thanh ve doi)
    QImage imgA(imgW, imgH, QImage::Format_ARGB32);
    imgA.fill(Qt::white);
    {
        OwnAnnotHideGuard hide(page, true);
        FPDF_BITMAP bmp = FPDFBitmap_CreateEx(imgW, imgH, FPDFBitmap_BGRA,
                                              imgA.bits(), imgA.bytesPerLine());
        if (!bmp) { FPDF_ClosePage(page); return false; }
        FPDF_RenderPageBitmap(bmp, page, 0, 0, imgW, imgH, 0, flagsBase | FPDF_ANNOT);
        FPDFBitmap_Destroy(bmp);
    }

    FPDF_ClosePage(page);

    // Tru pixel. PDFium tat dinh nen cho nao noi dung khong doi thi 2 anh giong HET
    // => so bang nhau tuyet doi, khong can nguong.
    m_img = QImage(imgW, imgH, QImage::Format_ARGB32);
    m_img.fill(Qt::transparent);
    qint64 diffPx = 0;
    for (int y = 0; y < imgH; ++y) {
        const QRgb* ra = reinterpret_cast<const QRgb*>(imgA.constScanLine(y));
        const QRgb* rb = reinterpret_cast<const QRgb*>(imgB.constScanLine(y));
        QRgb*       ro = reinterpret_cast<QRgb*>(m_img.scanLine(y));
        for (int x = 0; x < imgW; ++x) {
            if ((ra[x] & 0x00FFFFFF) == (rb[x] & 0x00FFFFFF)) continue;
            ro[x] = qRgba(qRed(ra[x]), qGreen(ra[x]), qBlue(ra[x]), 255);
            ++diffPx;
        }
    }

    m_ready = (diffPx > 0);
    qDebug().noquote() << "[fgnlayer] page=" << pageIndex
                       << "size=" << imgW << "x" << imgH
                       << "diffPx=" << diffPx << "ready=" << m_ready
                       << "ms=" << t.elapsed();
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

    QElapsedTimer t; t.start();

    FPDF_PAGE page = FPDF_LoadPage(doc, pageIndex);
    if (!page) return false;

    const double w = FPDF_GetPageWidth(page);
    const double h = FPDF_GetPageHeight(page);
    const int fullW = (std::max)(1, int(w * scale));
    const int fullH = (std::max)(1, int(h * scale));
    const int rw = regionPx.width();
    const int rh = regionPx.height();

    const int flagsBase = FPDF_RENDER_LIMITEDIMAGECACHE;

    // B — KHONG annot
    QImage imgB(rw, rh, QImage::Format_ARGB32);
    imgB.fill(Qt::white);
    {
        FPDF_BITMAP bmp = FPDFBitmap_CreateEx(rw, rh, FPDFBitmap_BGRA,
                                              imgB.bits(), imgB.bytesPerLine());
        if (!bmp) { FPDF_ClosePage(page); return false; }
        FPDF_RenderPageBitmap(bmp, page, -regionPx.x(), -regionPx.y(),
                              fullW, fullH, 0, flagsBase);
        FPDFBitmap_Destroy(bmp);
    }

    // A — CO annot, an annot cua chinh TorReader
    QImage imgA(rw, rh, QImage::Format_ARGB32);
    imgA.fill(Qt::white);
    {
        OwnAnnotHideGuard hide(page, true);
        FPDF_BITMAP bmp = FPDFBitmap_CreateEx(rw, rh, FPDFBitmap_BGRA,
                                              imgA.bits(), imgA.bytesPerLine());
        if (!bmp) { FPDF_ClosePage(page); return false; }
        FPDF_RenderPageBitmap(bmp, page, -regionPx.x(), -regionPx.y(),
                              fullW, fullH, 0, flagsBase | FPDF_ANNOT);
        FPDFBitmap_Destroy(bmp);
    }

    FPDF_ClosePage(page);

    m_regImg = QImage(rw, rh, QImage::Format_ARGB32);
    m_regImg.fill(Qt::transparent);
    qint64 diffPx = 0;
    for (int y = 0; y < rh; ++y) {
        const QRgb* ra = reinterpret_cast<const QRgb*>(imgA.constScanLine(y));
        const QRgb* rb = reinterpret_cast<const QRgb*>(imgB.constScanLine(y));
        QRgb*       ro = reinterpret_cast<QRgb*>(m_regImg.scanLine(y));
        for (int x = 0; x < rw; ++x) {
            if ((ra[x] & 0x00FFFFFF) == (rb[x] & 0x00FFFFFF)) continue;
            ro[x] = qRgba(qRed(ra[x]), qGreen(ra[x]), qBlue(ra[x]), 255);
            ++diffPx;
        }
    }

    m_regReady = true;
    qDebug().noquote() << "[fgnlayer] REGION page=" << pageIndex
                       << "rect=" << regionPx << "scale=" << scale
                       << "diffPx=" << diffPx << "ms=" << t.elapsed();
    return true;
}
