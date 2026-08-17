#pragma once
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QVector>
#include <QHash>
#include <fpdfview.h>
#include <fpdf_text.h>

// Chon chu theo CHI SO KY TU (kieu Adobe) — boc FPDFText_*, khong dinh giao dien.
// Moi loi goi PDFium deu gio QMutexLocker lock(&s_pdfiumMutex).
//
// Toa do dau vao cua charIndexAt/nearestCharAt la "PDF user space" (Y-up, goc
// duoi trai media box, CHUA ap /Rotate, CHUA tru CropBox) — cung khong gian ma
// FPDFText_GetCharBox/FPDFText_GetRect tra ve (dung thu tu nhu TextSearch.cpp).
// Tu toa do HIEN THI (Y-down, goc trai tren, da ap /Rotate + CropBox) phai
// chuyen qua dispToPagePt() truoc.

namespace TextSelection {

// Thong tin TEXT page cua mot trang. FPDF_PAGE + FPDF_TEXTPAGE la MUON TU
// PageCache (chu so huu duy nhat, SPEC_PAGECACHE_CORE) — ben goi KHONG Close.
struct PageInfo {
    FPDF_PAGE     page = nullptr;
    FPDF_TEXTPAGE tp   = nullptr;
    int           rot  = 0;      // FPDFPage_GetRotation & 3
    QPointF       box;           // CropBox (left, bottom)
    QSizeF        disp;          // kich thuoc hien thi (da ap /Rotate)
};

// Nap trang + text page (neu can) tu PageCache. GOI TU LUONG NEN / duong bam
// chuot (press, Ctrl+A, range). Co the mat ~1s cho trang CAD nang.
PageInfo pageFor(FPDF_DOCUMENT doc, int page);

// CHI doc dem, KHONG nap, KHONG cham s_pdfiumMutex — dung duoc o mouseMove.
// Tra rong neu trang chua san (con tro giu mui ten, khong chan giao dien).
PageInfo pageForCached(FPDF_DOCUMENT doc, int page);

// Doc dong / trang thay doi noi dung: xoa text page cu (va page) khoi PageCache.
void closePage(FPDF_DOCUMENT doc, int page);
void closeDocument(FPDF_DOCUMENT doc);

// Chuyen diem hien thi (Y-down, goc trai tren, da ap /Rotate + CropBox) sang
// PDF user space — dau vao cua charIndexAt.
QPointF dispToPagePt(const PageInfo& info, const QPointF& disp);
// Chuyen rect PDF user space sang toa do hien thi (de ve highlight giong search).
QRectF pageRectToDispPt(const PageInfo& info, const QRectF& r);

// Chi so ky tu tai diem (PDF user space). tol tinh point, nen suy tu zoom de o
// zoom nho van bam duoc chu (tol = so pixel / zoom). Tra -1 neu khong trung.
int charIndexAt(FPDF_TEXTPAGE tp, double x, double y, double tolX, double tolY);

// Neu charIndexAt khong trung: lay ky tu GAN NHAT theo phuong ngang tren CUNG
// DONG (cung baseline). Tra -1 neu khong co dong nao khe (vung rong trang).
int nearestCharAt(FPDF_TEXTPAGE tp, double x, double y, double tolY);

// Hop bao cua doan [start, start+count) — MOT rect moi DONG. BAT BUOC dung
// FPDFText_CountRects + FPDFText_GetRect, KHONG tu gop GetCharBox (se cat dau
// tieng Viet, da ghi trong TextSearch.cpp). Tra ve rect PDF user space.
QVector<QRectF> rectsForRange(FPDF_TEXTPAGE tp, int start, int count);

// Doan [start, start+count) cua trang info.page — rect toa do HIEN THI.
QVector<QRectF> rectsForRangeDisp(const PageInfo& info, int start, int count);

QString textForRange(FPDF_TEXTPAGE tp, int start, int count);

// Bien tu / bien dong quanh mot ky tu (cho nhay dup / nhay ba).
void wordRange(FPDF_TEXTPAGE tp, int idx, int* start, int* count);
void lineRange(FPDF_TEXTPAGE tp, int idx, int* start, int* count);

// Rects cua doan (anchorPage,anchorChar)-(focusPage,focusChar) chia theo trang,
// toa do HIEN THI. anchor/focus phai da chuan hoa theo thu tu doc (anchor<=focus).
// Trang dau tu anchorChar den het, trang giua toan bo, trang cuoi tu dau den
// focusChar.
QHash<int, QVector<QRectF>> rangeRectsByPageDisp(FPDF_DOCUMENT doc,
                                                 int anchorPage, int anchorChar,
                                                 int focusPage, int focusChar);

// Chu cua ca doan tren (nhieu trang noi voi nhau, giua trang them "\n").
QString rangeText(FPDF_DOCUMENT doc,
                  int anchorPage, int anchorChar,
                  int focusPage, int focusChar);

}  // namespace TextSelection
