#pragma once
#include <QPointF>
#include <QRectF>

// Rotation-aware coordinate transforms for PDF pages.
// (xu, yu) = unrotated PDF: Y up, origin bottom-left.
// (xd, yd) = display: Y down, origin top-left (after applying /Rotate).
// Wd, Hd = display size = FPDF_GetPageWidth/Height (already rotated).
// rot = FPDFPage_GetRotation: 0/1/2/3 = 0/90/180/270 deg CCW.

// Unrotated PDF → display (Y-down, rotation applied).
inline QPointF pdfToDisp(double xu, double yu, double Wd, double Hd, int rot) {
    switch (rot) {
        case 1:  return { yu,       xu };
        case 2:  return { Wd - xu,  yu };
        case 3:  return { Hd - yu,  Wd - xu };
        default: return { xu,       Hd - yu };
    }
}

inline QRectF pdfRectToDisp(const QRectF& r, double Wd, double Hd, int rot) {
    QPointF tl = pdfToDisp(r.left(), r.top(), Wd, Hd, rot);
    QPointF br = pdfToDisp(r.right(), r.bottom(), Wd, Hd, rot);
    return QRectF(tl, br).normalized();
}

// Display → unrotated PDF (inverse of pdfToDisp).
inline QPointF dispToPdf(double xd, double yd, double Wd, double Hd, int rot) {
    switch (rot) {
        case 1:  return { yd,       xd };
        case 2:  return { Wd - xd,  yd };
        case 3:  return { Hd - yd,  Wd - xd };
        default: return { xd,       Hd - yd };
    }
}
