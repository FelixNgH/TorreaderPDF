#pragma once
#include <QVector>
#include <fpdfview.h>
#include <fpdf_annot.h>

// MOT ham duy nhat quyet dinh annot nao do OVERLAY ve (khong duoc co trong anh nen).
// true: annot nay la markup cua ta (co TRUID), subtype thuoc tap drawable (overlay ve duoc),
//       va khong mang co FPDF_ANNOT_FLAG_HIDDEN.
inline bool isOverlayDrawnAnnot(FPDF_ANNOTATION a) {
    if (!a) return false;
    if (!FPDFAnnot_HasKey(a, "TRUID")) return false;
    if (FPDFAnnot_GetFlags(a) & FPDF_ANNOT_FLAG_HIDDEN) return false;
    const int sub = FPDFAnnot_GetSubtype(a);
    return sub == FPDF_ANNOT_INK || sub == FPDF_ANNOT_SQUARE ||
           sub == FPDF_ANNOT_CIRCLE || sub == FPDF_ANNOT_HIGHLIGHT ||
           sub == FPDF_ANNOT_LINE || sub == FPDF_ANNOT_POLYGON ||
           sub == FPDF_ANNOT_STAMP;
}

class OwnAnnotHideGuard {
    FPDF_PAGE m_page;
    QVector<int> m_hidden;
    bool m_active;
    bool m_overlayOnly; // true: hide chi annot isOverlayDrawnAnnot; false: hide moi TRUID
public:
    explicit OwnAnnotHideGuard(FPDF_PAGE page, bool active, bool overlayOnly = false)
        : m_page(page), m_active(active), m_overlayOnly(overlayOnly) {
        if (!m_active || !m_page) return;
        const int n = FPDFPage_GetAnnotCount(m_page);
        for (int i = 0; i < n; ++i) {
            FPDF_ANNOTATION a = FPDFPage_GetAnnot(m_page, i);
            if (!a) continue;
            const int flags = FPDFAnnot_GetFlags(a);
            const bool shouldHide = m_overlayOnly
                ? isOverlayDrawnAnnot(a)
                : (FPDFAnnot_HasKey(a, "TRUID") && !(flags & FPDF_ANNOT_FLAG_HIDDEN));
            if (shouldHide) {
                FPDFAnnot_SetFlags(a, flags | FPDF_ANNOT_FLAG_HIDDEN);
                m_hidden.append(i);
            }
            FPDFPage_CloseAnnot(a);
        }
    }
    ~OwnAnnotHideGuard() {
        if (!m_active) return;
        for (int i : m_hidden) {
            FPDF_ANNOTATION a = FPDFPage_GetAnnot(m_page, i);
            if (!a) continue;
            FPDFAnnot_SetFlags(a, FPDFAnnot_GetFlags(a) & ~FPDF_ANNOT_FLAG_HIDDEN);
            FPDFPage_CloseAnnot(a);
        }
    }
};
