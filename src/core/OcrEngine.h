#pragma once
#include <QString>
#include <QRectF>
#include <QVector>
#include <functional>
#include <fpdfview.h>

// Mot tu nhan duoc boi OCR. boxPt: toa do TRANG (unrotated PDF user space,
// goc duoi-trai, don vi point) — cung he toa do FPDFText_* va
// FPDFPageObj_SetMatrix su dung.
struct OcrWord {
    QString text;
    QRectF  boxPt;
    float   conf = 0.0f;
    // Tu thuoc dong nao (so thu tu doc, bat dau 0) va hop bao cua CA DONG
    // do. OcrTextLayer dung chieu cao dong lam co chu CHUNG cho moi tu trong
    // dong — tu co dau / khong dau khong con cao thap khac nhau.
    int     lineIndex = -1;
    QRectF  lineBoxPt;
};

// May nhan chu Tesseract. Giu stub khi build thieu tesseract: moi ham
// tra ve rong/false kem ly do de giao dien hien thi.
class OcrEngine {
public:
    // Co thu vien + traineddata khong. Ghi ly do vao whyNot (neu con tro khac null).
    static bool available(QString* whyNot);

    // Nhan dang mot trang: dung PDFium render trang thanh bitmap thang xam o
    // dpi cho truoc, dua vao Tesseract, tra ve danh sach tu (hinh chu nhat
    // tinh theo point, bo tu conf thap (< kMinConf) va tu rong). isCancelled: goi de kiem
    // huy, tra true la dung. Doc KHONG bi sua — day la du lieu de OcrTextLayer.
    static QVector<OcrWord> recognizePage(FPDF_DOCUMENT doc, int pageIndex,
                                          const QString& langs, int dpi,
                                          std::function<bool()> isCancelled);

    // Thu muc chua eng.traineddata/vie.traineddata. Uu tien TESSDATA_PREFIX,
    // roi thu muc tessdata canh file chay, roi resources/tessdata (dev).
    static QString tessdataDir();

    // Dang trong giao dien: nhan dang chay o luong nen, goi isCancelled de huy.
    static constexpr int kDefaultDpi = 300;
    static constexpr float kMinConf  = 30.0f;

    // Che do phan trang Tesseract mac dinh — so cua tesseract::PageSegMode
    // (khai bang int de header nay khong can header tesseract, ban dung khong
    // co tesseract van bien dich). 3 = PSM_AUTO, 11 = PSM_SPARSE_TEXT.
    // Chot 11 sau khi DO 3 cau hinh tren trang scan that: xem bang so trong
    // REPORT_OCR_QUALITY_R2_2026-08-16.md.
    static constexpr int kDefaultPsm = 11;
};
