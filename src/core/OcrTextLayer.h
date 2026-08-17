#pragma once
#include <QVector>
#include <fpdfview.h>
#include "OcrEngine.h"

// Chen lop chu vo hinh vao TAI LIEU PDFium DANG MO TRONG BO NHO.
// - Khong chua dong file de khi nguoi dung mo lai van giu nguyen.
// - Search/select/copy deu di qua FPDFText_* nen thay chu ngay (khong doi
//   TextSearch.cpp hay ContinuousView.cpp).
// - CHI can trang OCR roi: nguoi dung bam them cung khong chep hai lan.
class OcrTextLayer {
public:
    // Chen lop chu cho mot trang. font_size theo chieu cao hop tu. Tra ve
    // so luong tu da chen (0 neu trang da OCR hoac loi).
    static int insertPage(FPDF_DOCUMENT doc, int pageIndex,
                          const QVector<OcrWord>& words);

    // Trang nay da OCR chua? (de khong lam hai lan).
    static bool pageDone(FPDF_DOCUMENT doc, int pageIndex);

    // Goi khi dong tai lieu de don bo nho nhan dien trang cu.
    static void forgetDocument(FPDF_DOCUMENT doc);
};
