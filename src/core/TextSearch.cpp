#include "TextSearch.h"
#include <QtConcurrent>
#include <QMutex>
#include <fpdf_text.h>

extern QMutex s_pdfiumMutex;

TextSearch::TextSearch(QObject* parent) : QObject(parent) {}

void TextSearch::cancel() { m_cancelled = true; }

void TextSearch::search(PdfDocument* doc, const QString& query, Qt::CaseSensitivity cs) {
    if (!doc || !doc->isOpen() || query.isEmpty()) return;
    m_cancelled = false;

    FPDF_DOCUMENT rawDoc = doc->raw();
    int totalPages = doc->pageCount();

    QtConcurrent::run([this, rawDoc, query, cs, totalPages]() {
        int total = 0;
        for (int i = 0; i < totalPages && !m_cancelled; ++i) {
            auto results = searchPage(rawDoc, i, query, cs);
            for (auto& r : results) {
                emit found(r);
                ++total;
            }
            emit progress(i + 1, totalPages);
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

    FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
    if (!textPage) { FPDF_ClosePage(page); return results; }

    unsigned long flags = cs == Qt::CaseSensitive ? FPDF_MATCHCASE : 0;
    // utf16() returns const ushort* (2-byte) — correct for FPDF_WIDESTRING on all platforms.
    // std::wstring is 4-byte on Linux, causing garbled search if used here.
    FPDF_SCHHANDLE search = FPDFText_FindStart(
        textPage, reinterpret_cast<FPDF_WIDESTRING>(query.utf16()), flags, 0);

    while (FPDFText_FindNext(search)) {
        int charIdx = FPDFText_GetSchResultIndex(search);
        int charCount = FPDFText_GetSchCount(search);

        // Get bounding boxes of the match characters
        double left = 1e9, top = 1e9, right = -1e9, bottom = -1e9;
        for (int c = charIdx; c < charIdx + charCount; ++c) {
            double cl, ct, cr, cb;
            FPDFText_GetCharBox(textPage, c, &cl, &cr, &cb, &ct);
            left   = qMin(left, cl);
            right  = qMax(right, cr);
            top    = qMin(top, ct); // PDF Y grows up
            bottom = qMax(bottom, cb);
        }

        // Context snippet: up to 40 chars around the match
        int snippetStart = qMax(0, charIdx - 20);
        int snippetLen = charCount + 40;
        std::vector<unsigned short> buf(snippetLen + 1, 0);
        FPDFText_GetText(textPage, snippetStart, snippetLen, buf.data());
        QString snippet = QString::fromUtf16(buf.data());

        results.append({pageIndex, QRectF(left, top, right - left, bottom - top), snippet.trimmed()});
    }

    FPDFText_FindClose(search);
    FPDFText_ClosePage(textPage);
    FPDF_ClosePage(page);
    return results;
}
