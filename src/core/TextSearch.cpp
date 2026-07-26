#include "TextSearch.h"
#include "PdfCoords.h"
#include <QtConcurrent>
#include <QMutex>
#include <QThread>
#include <fpdf_text.h>
#include <fpdf_edit.h>

extern QMutex s_pdfiumMutex;

TextSearch::TextSearch(QObject* parent) : QObject(parent) {}

void TextSearch::cancel() { m_cancelled.storeRelaxed(1); }

void TextSearch::search(PdfDocument* doc, const QString& query, Qt::CaseSensitivity cs) {
    if (!doc || !doc->isOpen() || query.isEmpty()) return;
    m_cancelled.storeRelaxed(0);

    FPDF_DOCUMENT rawDoc = doc->raw();
    int totalPages = doc->pageCount();

    QtConcurrent::run([this, rawDoc, query, cs, totalPages]() {
        int total = 0;
        const int kMaxMatches = 2000;
        for (int i = 0; i < totalPages && !m_cancelled.loadRelaxed()
             && total < kMaxMatches; ++i) {
            auto results = searchPage(rawDoc, i, query, cs);
            for (auto& r : results) {
                if (total >= kMaxMatches) break;
                emit found(r);
                ++total;
            }
            emit progress(i + 1, totalPages);
            QThread::yieldCurrentThread();
        }
        emit searchComplete(total);
    });
}

QList<SearchResult> TextSearch::searchPage(FPDF_DOCUMENT doc, int pageIndex,
                                            const QString& query, Qt::CaseSensitivity cs) {
    QList<SearchResult> results;
    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = FPDF_LoadPage(doc, pageIndex);
    if (!page) return results;

    int pageRot = FPDFPage_GetRotation(page);
    double dispW = FPDF_GetPageWidth(page);
    double dispH = FPDF_GetPageHeight(page);

    FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
    if (!textPage) { FPDF_ClosePage(page); return results; }

    unsigned long flags = cs == Qt::CaseSensitive ? FPDF_MATCHCASE : 0;
    FPDF_SCHHANDLE search = FPDFText_FindStart(
        textPage, reinterpret_cast<FPDF_WIDESTRING>(query.utf16()), flags, 0);

    while (FPDFText_FindNext(search)) {
        int charIdx = FPDFText_GetSchResultIndex(search);
        int charCount = FPDFText_GetSchCount(search);

        // Unrotated PDF: Y grows UP, so top = max Y, bottom = min Y.
        double left = 1e9, top = -1e9, right = -1e9, bottom = 1e9;
        for (int c = charIdx; c < charIdx + charCount; ++c) {
            double cl, ct, cr, cb;
            FPDFText_GetCharBox(textPage, c, &cl, &cr, &cb, &ct);
            left   = qMin(left, cl);
            right  = qMax(right, cr);
            top    = qMax(top, ct);     // top = highest Y
            bottom = qMin(bottom, cb);  // bottom = lowest Y
        }

        // Convert to display coordinates (Y-down, rotation applied)
        QRectF dispRect = pdfRectToDisp(QRectF(left, bottom, right - left, top - bottom),
                                        dispW, dispH, pageRot);

        // Context snippet: up to 40 chars around the match
        int snippetStart = qMax(0, charIdx - 20);
        int snippetLen = charCount + 40;
        std::vector<unsigned short> buf(snippetLen + 1, 0);
        FPDFText_GetText(textPage, snippetStart, snippetLen, buf.data());
        QString snippet = QString::fromUtf16(buf.data());

        results.append({pageIndex, dispRect, snippet.trimmed()});
    }

    FPDFText_FindClose(search);
    FPDFText_ClosePage(textPage);
    FPDF_ClosePage(page);
    return results;
}
