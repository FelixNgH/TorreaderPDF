#include "PdfPrinter.h"
#include <QPainter>
#include <QPrinter>
#include <fpdfview.h>

PdfPrinter::PdfPrinter(QObject* parent) : QObject(parent) {}

void PdfPrinter::printPage(FPDF_DOCUMENT doc, QPrinter* printer, int pageIdx) {
    FPDF_PAGE page = FPDF_LoadPage(doc, pageIdx);
    if (!page) return;

    double pageW = FPDF_GetPageWidth(page);
    double pageH = FPDF_GetPageHeight(page);
    int dpi = printer->resolution();
    int pixW = static_cast<int>(pageW / 72.0 * dpi);
    int pixH = static_cast<int>(pageH / 72.0 * dpi);

    QImage img(pixW, pixH, QImage::Format_RGB888);
    img.fill(Qt::white);

    FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(
        pixW, pixH, FPDFBitmap_BGR, img.bits(), img.bytesPerLine());
    if (!bitmap) { FPDF_ClosePage(page); return; } // guard: alloc failure
    FPDF_RenderPageBitmap(bitmap, page, 0, 0, pixW, pixH, 0, 0);
    FPDFBitmap_Destroy(bitmap);
    FPDF_ClosePage(page);

    QPainter painter(printer);
    QRectF pageRect = printer->pageRect(QPrinter::DevicePixel);
    // Scale image to fit printer page, keep aspect ratio
    QSizeF scaled = QSizeF(pixW, pixH).scaled(pageRect.size(), Qt::KeepAspectRatio);
    QRectF dst(pageRect.topLeft(), scaled);
    painter.drawImage(dst, img);
}

void PdfPrinter::printRange(FPDF_DOCUMENT doc, QPrinter* printer, int first, int last) {
    int total = last - first + 1;
    for (int i = first; i <= last; ++i) {
        if (i > first) printer->newPage();
        printPage(doc, printer, i);
        emit progress(i - first + 1, total);
    }
}

void PdfPrinter::printAll(FPDF_DOCUMENT doc, QPrinter* printer) {
    int n = FPDF_GetPageCount(doc);
    if (n > 0) printRange(doc, printer, 0, n - 1);
}
