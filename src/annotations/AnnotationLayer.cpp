#include "AnnotationLayer.h"
#include <fpdf_edit.h>
#include <QString>
#include <QMutex>

extern QMutex s_pdfiumMutex;

AnnotationLayer::AnnotationLayer(QObject* parent) : QObject(parent) {}

void AnnotationLayer::setDocument(FPDF_DOCUMENT doc) { m_doc = doc; }

void AnnotationLayer::commitAnnotation(int pageIndex, AnnotTool tool, const AnnotStyle& style,
                                       QPointF start, QPointF end,
                                       const QVector<QPointF>& /*freehand*/) {
    if (!m_doc) return;

    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = FPDF_LoadPage(m_doc, pageIndex);
    if (!page) return;

    double pageH = FPDF_GetPageHeight(page);

    // Map AnnotTool → PDFium subtype
    FPDF_ANNOTATION_SUBTYPE subtype = FPDF_ANNOT_UNKNOWN;
    switch (tool) {
        case AnnotTool::Arrow:
        case AnnotTool::Line:          subtype = FPDF_ANNOT_LINE;      break;
        case AnnotTool::Rectangle:     subtype = FPDF_ANNOT_SQUARE;    break;
        case AnnotTool::Ellipse:       subtype = FPDF_ANNOT_CIRCLE;    break;
        case AnnotTool::TextComment:   subtype = FPDF_ANNOT_TEXT;      break;
        case AnnotTool::Highlight:     subtype = FPDF_ANNOT_HIGHLIGHT; break;
        case AnnotTool::Underline:     subtype = FPDF_ANNOT_UNDERLINE; break;
        case AnnotTool::Strikethrough: subtype = FPDF_ANNOT_STRIKEOUT; break;
        default:
            FPDF_ClosePage(page);
            return;
    }

    FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page, subtype);
    if (!annot) { FPDF_ClosePage(page); return; }

    // Y-flip: PDF Y increases upward, Qt Y increases downward
    FS_RECTF rect{
        static_cast<float>(qMin(start.x(), end.x())),
        static_cast<float>(pageH - qMax(start.y(), end.y())),
        static_cast<float>(qMax(start.x(), end.x())),
        static_cast<float>(pageH - qMin(start.y(), end.y()))
    };
    FPDFAnnot_SetRect(annot, &rect);

    // Set stroke color — PDFium API: (annot, type, R, G, B, A) all unsigned int 0-255
    // fix: correct API signature, use strokeColor not style.color
    unsigned int r = style.strokeColor.red();
    unsigned int g = style.strokeColor.green();
    unsigned int b = style.strokeColor.blue();
    unsigned int a = static_cast<unsigned int>(style.opacity * 255);
    FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, r, g, b, a);

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

    emit annotationAdded(pageIndex);
}
