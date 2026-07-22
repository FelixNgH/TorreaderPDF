#include "AnnotationLayer.h"
#include "AnnotationManager.h"
#include <fpdf_edit.h>
#include <QString>
#include <QMutex>
#include <cmath>
#include <vector>
#include <cstdio>

extern QMutex s_pdfiumMutex;

// ── Rotation-aware point transform ─────────────────────────────────────────────
// (xd, yd) = display coordinates (Y down from top of DISPLAYED page)
// (Wd, Hd) = display size = FPDF_GetPageWidth/Height
// returns  (xu, yu) = unrotated PDF space (Y up from bottom)
static QPointF dispToPdf(double xd, double yd, double Wd, double Hd, int rot) {
    switch (rot) {
        case 1:  return { yd,       xd };
        case 2:  return { Wd - xd,  yd };
        case 3:  return { Hd - yd,  Wd - xd };
        default: return { xd,       Hd - yd };   // 0
    }
}

AnnotationLayer::AnnotationLayer(QObject* parent) : QObject(parent) {}

void AnnotationLayer::setDocument(FPDF_DOCUMENT doc) { m_doc = doc; }

void AnnotationLayer::commitAnnotation(int pageIndex, AnnotTool tool, const AnnotStyle& style,
                                       QPointF start, QPointF end,
                                       const QVector<QPointF>& /*freehand*/) {
    if (!m_doc) return;

    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = FPDF_LoadPage(m_doc, pageIndex);
    if (!page) return;

    double pageH  = FPDF_GetPageHeight(page);
    double pageW  = FPDF_GetPageWidth(page);
    int    rot    = FPDFPage_GetRotation(page);

    // Line & Arrow → INK annotation. A bare FPDF_ANNOT_LINE (no /L, no AP) is dropped on save.
    if (tool == AnnotTool::Line || tool == AnnotTool::Arrow) {
        FPDF_ANNOTATION ink = FPDFPage_CreateAnnot(page, FPDF_ANNOT_INK);
        if (!ink) { FPDF_ClosePage(page); return; }
        QPointF pa = dispToPdf(start.x(), start.y(), pageW, pageH, rot);
        QPointF pb = dispToPdf(end.x(), end.y(), pageW, pageH, rot);
        FS_POINTF a{ static_cast<float>(pa.x()), static_cast<float>(pa.y()) };
        FS_POINTF b{ static_cast<float>(pb.x()), static_cast<float>(pb.y()) };
        FS_POINTF shaft[2] = { a, b };
        FPDFAnnot_AddInkStroke(ink, shaft, 2);
        if (tool == AnnotTool::Arrow) {
            float dx = b.x - a.x, dy = b.y - a.y;
            float len = std::sqrt(dx*dx + dy*dy);
            if (len > 0.1f) {
                float ang = std::atan2(dy, dx);
                const float hl = 18.0f;
                const float d25 = 25.0f * 3.14159265f / 180.0f;
                FS_POINTF w1{ static_cast<float>(b.x - hl * std::cos(ang - d25)),
                              static_cast<float>(b.y - hl * std::sin(ang - d25)) };
                FS_POINTF w2{ static_cast<float>(b.x - hl * std::cos(ang + d25)),
                              static_cast<float>(b.y - hl * std::sin(ang + d25)) };
                FS_POINTF head[3] = { w1, b, w2 };
                FPDFAnnot_AddInkStroke(ink, head, 3);
            }
        }
        FS_RECTF rr{
            static_cast<float>(qMin(a.x, b.x) - 5.0f),
            static_cast<float>(qMax(a.y, b.y) + 5.0f),
            static_cast<float>(qMax(a.x, b.x) + 5.0f),
            static_cast<float>(qMin(a.y, b.y) - 5.0f)
        };
        FPDFAnnot_SetRect(ink, &rr);
        unsigned int ir = style.strokeColor.red();
        unsigned int ig = style.strokeColor.green();
        unsigned int ib = style.strokeColor.blue();
        unsigned int ia = static_cast<unsigned int>(style.opacity * 255);
        FPDFAnnot_SetColor(ink, FPDFANNOT_COLORTYPE_Color, ir, ig, ib, ia);
        {
            QString trc = QString("%1,%2,%3").arg(ir).arg(ig).arg(ib);
            FPDFAnnot_SetStringValue(ink, "TRC", reinterpret_cast<FPDF_WIDESTRING>(trc.utf16()));
        }
        FPDFAnnot_SetBorder(ink, 0.0f, 0.0f, style.strokeWidth);
        FPDFPage_CloseAnnot(ink);
        FPDFPage_GenerateContent(page);
        FPDF_ClosePage(page);
        lock.unlock();
        emit annotationAdded(pageIndex);
        return;
    }

    // Cloud → INK annotation tracing a scalloped (cloud) outline. Reliable render like Line/Arrow.
    if (tool == AnnotTool::Cloud) {
        FPDF_ANNOTATION ck = FPDFPage_CreateAnnot(page, FPDF_ANNOT_INK);
        if (!ck) { FPDF_ClosePage(page); return; }
        QPointF pa = dispToPdf(start.x(), start.y(), pageW, pageH, rot);
        QPointF pb = dispToPdf(end.x(), end.y(), pageW, pageH, rot);
        float x0 = static_cast<float>(qMin(pa.x(), pb.x()));
        float x1 = static_cast<float>(qMax(pa.x(), pb.x()));
        float yBot = static_cast<float>(qMin(pa.y(), pb.y()));
        float yTop = static_cast<float>(qMax(pa.y(), pb.y()));
        const float r = 9.0f;
        std::vector<FS_POINTF> pts;
        auto addEdge = [&](float ax, float ay, float bx, float by, float nx, float ny) {
            float dx = bx - ax, dy = by - ay;
            float len = std::sqrt(dx * dx + dy * dy);
            if (len < 1.0f) return;
            int bumps = qMax(1, static_cast<int>(len / (2.0f * r)));
            float ux = dx / len, uy = dy / len;
            float seg = len / bumps;
            for (int i = 0; i < bumps; ++i) {
                float sx = ax + ux * seg * i, sy = ay + uy * seg * i;
                const int steps = 6;
                for (int k = 0; k <= steps; ++k) {
                    float t = static_cast<float>(k) / steps;
                    float px = sx + ux * seg * t, py = sy + uy * seg * t;
                    float bulge = std::sin(t * 3.14159265f) * r;
                    pts.push_back(FS_POINTF{ px + nx * bulge, py + ny * bulge });
                }
            }
        };
        addEdge(x0, yBot, x1, yBot, 0.0f, -1.0f);
        addEdge(x1, yBot, x1, yTop, 1.0f, 0.0f);
        addEdge(x1, yTop, x0, yTop, 0.0f, 1.0f);
        addEdge(x0, yTop, x0, yBot, -1.0f, 0.0f);
        if (pts.size() >= 2)
            FPDFAnnot_AddInkStroke(ck, pts.data(), pts.size());
        FS_RECTF rr{ x0 - r - 2, yTop + r + 2, x1 + r + 2, yBot - r - 2 };
        FPDFAnnot_SetRect(ck, &rr);
        unsigned int cr = style.strokeColor.red();
        unsigned int cg = style.strokeColor.green();
        unsigned int cb = style.strokeColor.blue();
        FPDFAnnot_SetColor(ck, FPDFANNOT_COLORTYPE_Color, cr, cg, cb,
                           static_cast<unsigned int>(style.opacity * 255));
        {
            QString trc = QString("%1,%2,%3").arg(cr).arg(cg).arg(cb);
            FPDFAnnot_SetStringValue(ck, "TRC", reinterpret_cast<FPDF_WIDESTRING>(trc.utf16()));
        }
        FPDFAnnot_SetBorder(ck, 0.0f, 0.0f, style.strokeWidth);
        FPDFPage_CloseAnnot(ck);
        FPDFPage_GenerateContent(page);
        FPDF_ClosePage(page);
        lock.unlock();
        emit annotationAdded(pageIndex);
        return;
    }

    // Map AnnotTool → PDFium subtype
    FPDF_ANNOTATION_SUBTYPE subtype = FPDF_ANNOT_UNKNOWN;
    switch (tool) {
        case AnnotTool::Rectangle:     subtype = FPDF_ANNOT_SQUARE;    break;
        case AnnotTool::Ellipse:       subtype = FPDF_ANNOT_CIRCLE;    break;
        case AnnotTool::TextComment:   subtype = FPDF_ANNOT_TEXT;      break;
        case AnnotTool::Highlight:     subtype = FPDF_ANNOT_HIGHLIGHT; break;
        case AnnotTool::Underline:     subtype = FPDF_ANNOT_UNDERLINE; break;
        case AnnotTool::Strikethrough: subtype = FPDF_ANNOT_STRIKEOUT; break;
        case AnnotTool::FreeText:
            // Handled via noteRequested signal, not via PDF annotation
            FPDF_ClosePage(page);
            return;
        default:
            FPDF_ClosePage(page);
            return;
    }

    FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page, subtype);
    if (!annot) { FPDF_ClosePage(page); return; }

    QPointF pa = dispToPdf(start.x(), start.y(), pageW, pageH, rot);
    QPointF pb = dispToPdf(end.x(), end.y(), pageW, pageH, rot);
    FS_RECTF rect{
        static_cast<float>(qMin(pa.x(), pb.x())),
        static_cast<float>(qMin(pa.y(), pb.y())),
        static_cast<float>(qMax(pa.x(), pb.x())),
        static_cast<float>(qMax(pa.y(), pb.y()))
    };
    FPDFAnnot_SetRect(annot, &rect);

    // Set stroke color — PDFium API: (annot, type, R, G, B, A) all unsigned int 0-255
    // fix: correct API signature, use strokeColor not style.color
    unsigned int r = style.strokeColor.red();
    unsigned int g = style.strokeColor.green();
    unsigned int b = style.strokeColor.blue();
    unsigned int a = static_cast<unsigned int>(style.opacity * 255);
    FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, r, g, b, a);
    {
        QString trc = QString("%1,%2,%3").arg(r).arg(g).arg(b);
        FPDFAnnot_SetStringValue(annot, "TRC", reinterpret_cast<FPDF_WIDESTRING>(trc.utf16()));
    }
    FPDFAnnot_SetBorder(annot, 0.0f, 0.0f, style.strokeWidth);
    if (style.fillColor.alpha() > 0)
        FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_InteriorColor,
                           style.fillColor.red(), style.fillColor.green(),
                           style.fillColor.blue(), style.fillColor.alpha());

    // Text comment: set content string
    // fix: key is "Contents" (FPDF_BYTESTRING), value is FPDF_WIDESTRING
    if (tool == AnnotTool::TextComment) {
        static const char16_t kNewComment[] = u"New Comment";
        FPDFAnnot_SetStringValue(annot, "Contents",
            reinterpret_cast<FPDF_WIDESTRING>(kNewComment));
    }

    FPDFPage_CloseAnnot(annot);         // must close annot before generating content
    FPDFPage_GenerateContent(page);
    FPDF_ClosePage(page);
    lock.unlock();

    emit annotationAdded(pageIndex);
}
