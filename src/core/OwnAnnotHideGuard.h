#pragma once
#include <QVector>
#include <fpdfview.h>
#include <fpdf_annot.h>

class OwnAnnotHideGuard {
    FPDF_PAGE m_page;
    QVector<int> m_hidden;
    bool m_active;
public:
    explicit OwnAnnotHideGuard(FPDF_PAGE page, bool active) : m_page(page), m_active(active) {
        if (!m_active || !m_page) return;
        const int n = FPDFPage_GetAnnotCount(m_page);
        for (int i = 0; i < n; ++i) {
            FPDF_ANNOTATION a = FPDFPage_GetAnnot(m_page, i);
            if (!a) continue;
            const int flags = FPDFAnnot_GetFlags(a);
            if (FPDFAnnot_HasKey(a, "TRUID") && !(flags & FPDF_ANNOT_FLAG_HIDDEN)) {
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
