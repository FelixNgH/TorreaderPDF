#pragma once
#include <QPointF>
#include <QRectF>
#include <fpdfview.h>
#include <fpdf_transformpage.h>

// Rotation-aware coordinate transforms for PDF pages.
// (xu, yu) = unrotated PDF: Y up, origin = GOC HOP TRANG (KHONG phai luon (0,0)).
// (xd, yd) = display: Y down, origin top-left (after applying /Rotate).
// Wd, Hd = display size = FPDF_GetPageWidth/Height (already rotated).
// rot = FPDFPage_GetRotation: 0/1/2/3 = 0/90/180/270 deg CCW.
// (ox, oy) = goc hop trang (CropBox.left, CropBox.bottom).

// 🔴 Trang xuat tu CAD/Revit thuong co hop lay TAM trang lam goc,
//    vd [-1191.97,-841.89,1191.97,841.89]. Khong tru goc nay thi MOI annot
//    va MOI highlight tim kiem lech DUNG NUA TRANG (bug 2026-08-07).
inline QPointF pdfBoxOrigin(FPDF_PAGE page) {
    float l = 0.f, b = 0.f, r = 0.f, t = 0.f;
    bool ok = FPDFPage_GetCropBox(page, &l, &b, &r, &t) != 0;
    if (!ok) ok = FPDFPage_GetMediaBox(page, &l, &b, &r, &t) != 0;
    if (!ok || r <= l || t <= b) return QPointF(0.0, 0.0);
    return QPointF(double(l), double(b));
}

// Unrotated PDF → display (Y-down, rotation applied).
inline QPointF pdfToDisp(double xu, double yu, double Wd, double Hd, int rot,
                         double ox = 0.0, double oy = 0.0) {
    xu -= ox;
    yu -= oy;
    switch (rot) {
        case 1:  return { yu,       xu };
        case 2:  return { Wd - xu,  yu };
        case 3:  return { Hd - yu,  Wd - xu };
        default: return { xu,       Hd - yu };
    }
}

inline QRectF pdfRectToDisp(const QRectF& r, double Wd, double Hd, int rot,
                            double ox = 0.0, double oy = 0.0) {
    QPointF tl = pdfToDisp(r.left(), r.top(), Wd, Hd, rot, ox, oy);
    QPointF br = pdfToDisp(r.right(), r.bottom(), Wd, Hd, rot, ox, oy);
    return QRectF(tl, br).normalized();
}

// Display → unrotated PDF (inverse of pdfToDisp).
inline QPointF dispToPdf(double xd, double yd, double Wd, double Hd, int rot,
                         double ox = 0.0, double oy = 0.0) {
    QPointF p;
    switch (rot) {
        case 1:  p = { yd,       xd };            break;
        case 2:  p = { Wd - xd,  yd };            break;
        case 3:  p = { Hd - yd,  Wd - xd };       break;
        default: p = { xd,       Hd - yd };       break;
    }
    return QPointF(p.x() + ox, p.y() + oy);
}
