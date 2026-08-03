#include "AnnotationLayer.h"
#include "AnnotationManager.h"
#include "../core/PdfCoords.h"
#include <fpdf_edit.h>
#include <fpdf_text.h>
#include <QString>
#include <QMutex>
#include <QMap>
#include <cmath>
#include <vector>
#include <cstdio>

extern QMutex s_pdfiumMutex;

AnnotationLayer::AnnotationLayer(QObject* parent) : QObject(parent) {}

void AnnotationLayer::setDocument(FPDF_DOCUMENT doc) { m_doc = doc; }

static const char* toolName(AnnotTool t) {
    switch (t) {
        case AnnotTool::Line:         return "Line";
        case AnnotTool::Arrow:        return "Arrow";
        case AnnotTool::Freehand:     return "Freehand";
        case AnnotTool::Cloud:        return "Cloud";
        case AnnotTool::Highlight:    return "Highlight";
        case AnnotTool::Rectangle:    return "Rectangle";
        case AnnotTool::Ellipse:      return "Ellipse";
        case AnnotTool::Underline:    return "Underline";
        case AnnotTool::Strikethrough:return "Strikethrough";
        case AnnotTool::TextComment:  return "Note";
        default:                      return "Annotation";
    }
}

void AnnotationLayer::commitAnnotation(int pageIndex, AnnotTool tool, const AnnotStyle& style,
                                       QPointF start, QPointF end,
                                       const QVector<QPointF>& freehand) {
    if (!m_doc) return;

    auto setUid = [&](FPDF_ANNOTATION a) {
        if (!m_annotMgr) return;
        m_lastCreatedUid = m_annotMgr->generateUid();
        FPDFAnnot_SetStringValue(a, "TRUID", reinterpret_cast<FPDF_WIDESTRING>(m_lastCreatedUid.utf16()));
    };
    auto setTool = [&](FPDF_ANNOTATION a) {
        QString tn = QString::fromLatin1(toolName(tool));
        FPDFAnnot_SetStringValue(a, "TRTOOL", reinterpret_cast<FPDF_WIDESTRING>(tn.utf16()));
    };

    QMutexLocker lock(&s_pdfiumMutex);
    const bool sharedPg = (m_annotMgr && m_annotMgr->isSharedPage(pageIndex));
    FPDF_PAGE page = sharedPg ? m_annotMgr->acquireSharedPage(pageIndex) : FPDF_LoadPage(m_doc, pageIndex);
    if (!page) return;

    double pageH  = FPDF_GetPageHeight(page);
    double pageW  = FPDF_GetPageWidth(page);
    int    rot    = FPDFPage_GetRotation(page);

    // Line & Arrow → INK annotation. A bare FPDF_ANNOT_LINE (no /L, no AP) is dropped on save.
    if (tool == AnnotTool::Line || tool == AnnotTool::Arrow) {
        FPDF_ANNOTATION ink = FPDFPage_CreateAnnot(page, FPDF_ANNOT_INK);
        if (!ink) { if (!sharedPg) FPDF_ClosePage(page); return; }
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
        setUid(ink);
        setTool(ink);
        AnnotVisual _av;
        bool _avOk = m_annotMgr && m_annotMgr->buildVisual(page, ink, pageIndex, _av);
        FPDFPage_CloseAnnot(ink);
        if (!sharedPg) FPDF_ClosePage(page);
        lock.unlock();
        if (m_annotMgr) m_annotMgr->bumpPageRevision(pageIndex);
        if (_avOk) emit annotVisualAdded(pageIndex, _av);
        emit annotationAdded(pageIndex);
        return;
    }

    // Freehand → INK annotation from collected points
    if (tool == AnnotTool::Freehand) {
        FPDF_ANNOTATION ink = FPDFPage_CreateAnnot(page, FPDF_ANNOT_INK);
        if (!ink) { if (!sharedPg) FPDF_ClosePage(page); return; }
        int n = freehand.size();
        if (n == 0) { FPDFPage_CloseAnnot(ink); if (!sharedPg) FPDF_ClosePage(page); return; }
        std::vector<FS_POINTF> pts(n);
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (int i = 0; i < n; ++i) {
            QPointF p = dispToPdf(freehand[i].x(), freehand[i].y(), pageW, pageH, rot);
            pts[i] = { static_cast<float>(p.x()), static_cast<float>(p.y()) };
            x0 = qMin(x0, pts[i].x); x1 = qMax(x1, pts[i].x);
            y0 = qMin(y0, pts[i].y); y1 = qMax(y1, pts[i].y);
        }
        FPDFAnnot_AddInkStroke(ink, pts.data(), n);
        FS_RECTF rr{ x0 - 3, y1 + 3, x1 + 3, y0 - 3 };
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
        setUid(ink);
        setTool(ink);
        AnnotVisual _av;
        bool _avOk = m_annotMgr && m_annotMgr->buildVisual(page, ink, pageIndex, _av);
        FPDFPage_CloseAnnot(ink);
        if (!sharedPg) FPDF_ClosePage(page);
        lock.unlock();
        if (m_annotMgr) m_annotMgr->bumpPageRevision(pageIndex);
        if (_avOk) emit annotVisualAdded(pageIndex, _av);
        emit annotationAdded(pageIndex);
        return;
    }

    // Cloud → INK annotation tracing a scalloped (cloud) outline. Reliable render like Line/Arrow.
    if (tool == AnnotTool::Cloud) {
        FPDF_ANNOTATION ck = FPDFPage_CreateAnnot(page, FPDF_ANNOT_INK);
        if (!ck) { if (!sharedPg) FPDF_ClosePage(page); return; }
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
        setUid(ck);
        setTool(ck);
        AnnotVisual _av;
        bool _avOk = m_annotMgr && m_annotMgr->buildVisual(page, ck, pageIndex, _av);
        FPDFPage_CloseAnnot(ck);
        if (!sharedPg) FPDF_ClosePage(page);
        lock.unlock();
        if (m_annotMgr) m_annotMgr->bumpPageRevision(pageIndex);
        if (_avOk) emit annotVisualAdded(pageIndex, _av);
        emit annotationAdded(pageIndex);
        return;
    }

    // Highlight with text-bound QuadPoints (fallback to rect if no text)
    if (tool == AnnotTool::Highlight) {
        FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page, FPDF_ANNOT_HIGHLIGHT);
        if (!annot) { if (!sharedPg) FPDF_ClosePage(page); return; }

        QPointF pa = dispToPdf(start.x(), start.y(), pageW, pageH, rot);
        QPointF pb = dispToPdf(end.x(), end.y(), pageW, pageH, rot);
        float l = static_cast<float>(qMin(pa.x(), pb.x()));
        float b = static_cast<float>(qMin(pa.y(), pb.y()));
        float r = static_cast<float>(qMax(pa.x(), pb.x()));
        float t = static_cast<float>(qMax(pa.y(), pb.y()));

        FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
        bool hasQuads = false;
        if (textPage) {
            int charCount = FPDFText_CountChars(textPage);
            QMap<double, QVector<int>> lineChars;
            for (int i = 0; i < charCount; ++i) {
                double cl, ct, cr, cb;
                FPDFText_GetCharBox(textPage, i, &cl, &cr, &cb, &ct);
                if (cl < r && cr > l && ct < t && cb > b) {
                    double key = std::round((ct + cb) / 10.0) * 10.0;
                    lineChars[key].append(i);
                }
            }
            if (!lineChars.isEmpty()) {
                hasQuads = true;
                float ql = l, qr = r, qt = t, qb = b;
                for (auto it = lineChars.constBegin(); it != lineChars.constEnd(); ++it) {
                    const auto& chars = it.value();
                    if (chars.isEmpty()) continue;
                    double ll = 1e9, lr = -1e9, lt = 1e9, lb = -1e9;
                    for (int idx : chars) {
                        double cl, ct, cr, cb;
                        FPDFText_GetCharBox(textPage, idx, &cl, &cr, &cb, &ct);
                        ll = qMin(ll, cl); lr = qMax(lr, cr);
                        lt = qMin(lt, ct); lb = qMax(lb, cb);
                    }
                    FS_QUADPOINTSF qp;
                    qp.x1 = static_cast<float>(ll); qp.y1 = static_cast<float>(lt);
                    qp.x2 = static_cast<float>(lr); qp.y2 = static_cast<float>(lt);
                    qp.x3 = static_cast<float>(ll); qp.y3 = static_cast<float>(lb);
                    qp.x4 = static_cast<float>(lr); qp.y4 = static_cast<float>(lb);
                    FPDFAnnot_AppendAttachmentPoints(annot, &qp);
                    ql = qMin(ql, static_cast<float>(ll)); qr = qMax(qr, static_cast<float>(lr));
                    qt = qMin(qt, static_cast<float>(lt)); qb = qMax(qb, static_cast<float>(lb));
                }
                l = ql; r = qr; t = qt; b = qb;
            }
            FPDFText_ClosePage(textPage);
        }

        // ponytail: guard against degenerate rect — min 6pt height, skip if near-zero area
        float rw = r - l, rh = t - b;
        if (rw < 1.0f && rh < 1.0f) {
            FPDFText_ClosePage(textPage);
            FPDFPage_CloseAnnot(annot);
            if (!sharedPg) FPDF_ClosePage(page);
            lock.unlock();
            return;
        }
        if (rh < 6.0f) { float pad = (6.0f - rh) / 2.0f; b -= pad; t += pad; }
        if (rw < 6.0f) { float pad = (6.0f - rw) / 2.0f; l -= pad; r += pad; }
        FS_RECTF rect{ l, b, r, t };
        FPDFAnnot_SetRect(annot, &rect);
        unsigned int cr = style.strokeColor.red();
        unsigned int cg = style.strokeColor.green();
        unsigned int cb = style.strokeColor.blue();
        if (cr == 255 && cg == 0 && cb == 0) { cr = 255; cg = 255; cb = 0; }
        FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, cr, cg, cb,
                           static_cast<unsigned int>(style.opacity * 255));
        {
            QString trc = QString("%1,%2,%3").arg(cr).arg(cg).arg(cb);
            FPDFAnnot_SetStringValue(annot, "TRC", reinterpret_cast<FPDF_WIDESTRING>(trc.utf16()));
        }
        setUid(annot);
        setTool(annot);
        AnnotVisual _av;
        bool _avOk = m_annotMgr && m_annotMgr->buildVisual(page, annot, pageIndex, _av);
        FPDFPage_CloseAnnot(annot);
        if (!sharedPg) FPDF_ClosePage(page);
        lock.unlock();
        if (m_annotMgr) m_annotMgr->bumpPageRevision(pageIndex);
        if (_avOk) emit annotVisualAdded(pageIndex, _av);
        emit annotationAdded(pageIndex);
        return;
    }

    // Map AnnotTool → PDFium subtype
    FPDF_ANNOTATION_SUBTYPE subtype = FPDF_ANNOT_UNKNOWN;
    switch (tool) {
        case AnnotTool::Rectangle:     subtype = FPDF_ANNOT_SQUARE;    break;
        case AnnotTool::Ellipse:       subtype = FPDF_ANNOT_CIRCLE;    break;
        case AnnotTool::TextComment:   subtype = FPDF_ANNOT_TEXT;      break;
        case AnnotTool::Underline:     subtype = FPDF_ANNOT_UNDERLINE; break;
        case AnnotTool::Strikethrough: subtype = FPDF_ANNOT_STRIKEOUT; break;
        case AnnotTool::FreeText:
            // Handled via noteRequested signal, not via PDF annotation
            if (!sharedPg) FPDF_ClosePage(page);
            return;
        default:
            if (!sharedPg) FPDF_ClosePage(page);
            return;
    }

    FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page, subtype);
    if (!annot) { if (!sharedPg) FPDF_ClosePage(page); return; }

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

    setUid(annot);
    setTool(annot);
    AnnotVisual _av;
    bool _avOk = m_annotMgr && m_annotMgr->buildVisual(page, annot, pageIndex, _av);
    FPDFPage_CloseAnnot(annot);
    if (!sharedPg) FPDF_ClosePage(page);
    lock.unlock();

    if (m_annotMgr) m_annotMgr->bumpPageRevision(pageIndex);
    if (_avOk) emit annotVisualAdded(pageIndex, _av);
    emit annotationAdded(pageIndex);
}
