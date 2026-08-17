#include "TextSearch.h"
#include "PdfCoords.h"
#include <QtConcurrent>
#include <QMutex>
#include <QThread>
#include <QVector>
#include <vector>
#include <fpdf_text.h>
#include <fpdf_edit.h>

extern QMutex s_pdfiumMutex;

TextSearch::TextSearch(QObject* parent) : QObject(parent) {}

void TextSearch::cancel() { m_cancelled.storeRelaxed(1); }

namespace {

// Bang anh xa chuoi da gap (bo dau) -> chi so ky tu GOC. Mot ky tu goc co the
// No RA nhieu diem sau NFD (vd "Ặ" -> A + dau to hop), nen bang nay ghi theo
// TUNG diem ma da gap (sau khi bo dau to hop Mn + thuong) ve chi so ky tu goc.
// 'd'/'D' (U+0111/U+0110) khong phai dau to hop — dich tay sang d/D.
struct FoldMap {
    QString folded;            // chuoi da gap: bo dau + thuong
    QVector<int> foldedToOrig; // folded[i] thuoc ky tu GOC nao
};

FoldMap buildFoldMap(const QString& text) {
    FoldMap out;
    out.foldedToOrig.reserve(text.size());
    for (int i = 0; i < text.size(); ) {
        const int origIdx = i;
        uint cp = text.at(i).unicode();
        int step = 1;
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < text.size()
            && text.at(i + 1).unicode() >= 0xDC00 && text.at(i + 1).unicode() <= 0xDFFF) {
            cp = QChar::surrogateToUcs4(text.at(i), text.at(i + 1));
            step = 2;
        }
        if (cp == 0x0111) cp = 0x0064;      // d
        else if (cp == 0x0110) cp = 0x0044; // D

        // PDFium FindStart BO QUA ky tu xuong dong khi so khop (PDFium chen
        // \r\n vao char list giua cac text run). Giong vay de khong sot ket
        // qua bi tach dong (CAD hay tach "MẶT" thanh "M\r\nẶT").
        if (cp == 0x000D || cp == 0x000A) { i += step; continue; }

        const char32_t oneCp = char32_t(cp);
        const QString nfd = QString::fromUcs4(&oneCp, 1).normalized(QString::NormalizationForm_D);
        for (const QChar& c : nfd) {
            if (QChar::category(c.unicode()) == QChar::Mark_NonSpacing)
                continue;
            const QString lower = QString(c).toLower();
            for (const QChar& lc : lower) {
                out.folded.append(lc);
                out.foldedToOrig.append(origIdx);
            }
        }
        i += step;
    }
    return out;
}

} // namespace

QString TextSearch::foldForMatch(const QString& text) {
    return buildFoldMap(text).folded;
}

void TextSearch::search(PdfDocument* doc, const QString& query, Qt::CaseSensitivity cs,
                        bool matchDiacritics) {
    if (!doc || !doc->isOpen() || query.isEmpty()) return;
    m_cancelled.storeRelaxed(0);

    FPDF_DOCUMENT rawDoc = doc->raw();
    int totalPages = doc->pageCount();

    QtConcurrent::run([this, rawDoc, query, cs, matchDiacritics, totalPages]() {
        int total = 0;
        const int kMaxMatches = 2000;
        for (int i = 0; i < totalPages && !m_cancelled.loadRelaxed()
             && total < kMaxMatches; ++i) {
            auto results = matchDiacritics ? searchPageExact(rawDoc, i, query, cs)
                                           : searchPageFolded(rawDoc, i, query, cs);
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

// Duong cu: FPDFText_FindStart, khop UTF-16 tho. Dung cho che do "Match
// diacritics" (khop chinh xac).
QList<SearchResult> TextSearch::searchPageExact(FPDF_DOCUMENT doc, int pageIndex,
                                                const QString& query, Qt::CaseSensitivity cs) {
    QList<SearchResult> results;
    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = FPDF_LoadPage(doc, pageIndex);
    if (!page) return results;

    int pageRot = FPDFPage_GetRotation(page);
    double dispW = FPDF_GetPageWidth(page);
    double dispH = FPDF_GetPageHeight(page);
    const QPointF box = pdfBoxOrigin(page);

    FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
    if (!textPage) { FPDF_ClosePage(page); return results; }

    unsigned long flags = cs == Qt::CaseSensitive ? FPDF_MATCHCASE : 0;
    FPDF_SCHHANDLE search = FPDFText_FindStart(
        textPage, reinterpret_cast<FPDF_WIDESTRING>(query.utf16()), flags, 0);

    while (FPDFText_FindNext(search)) {
        int charIdx = FPDFText_GetSchResultIndex(search);
        int charCount = FPDFText_GetSchCount(search);

        QVector<QRectF> rects;
        int n = FPDFText_CountRects(textPage, charIdx, charCount);
        for (int i = 0; i < n; ++i) {
            double left = 0, top = 0, right = 0, bottom = 0;
            if (!FPDFText_GetRect(textPage, i, &left, &top, &right, &bottom))
                continue;
            rects.append(pdfRectToDisp(QRectF(left, bottom, right - left, top - bottom),
                                       dispW, dispH, pageRot, box.x(), box.y()));
        }
        if (rects.isEmpty())
            continue;

        int snippetStart = qMax(0, charIdx - 20);
        int snippetLen = charCount + 40;
        std::vector<unsigned short> buf(snippetLen + 1, 0);
        FPDFText_GetText(textPage, snippetStart, snippetLen, buf.data());
        QString snippet = QString::fromUtf16(buf.data());

        results.append({pageIndex, rects, snippet.trimmed(), charIdx, charCount});
    }

    FPDFText_FindClose(search);
    FPDFText_ClosePage(textPage);
    FPDF_ClosePage(page);
    return results;
}

// Che do bo dau: lay toan van trang, gap ve dang so khop (bo dau + thuong),
// gio bang anh xa chi so GAP -> chi so GOC, tim tren chuoi da gap, roi doi
// nguoc ra charIdx/charCount de FPDFText_CountRects/GetRect nhu binh thuong.
QList<SearchResult> TextSearch::searchPageFolded(FPDF_DOCUMENT doc, int pageIndex,
                                                 const QString& query, Qt::CaseSensitivity cs) {
    QList<SearchResult> results;
    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = FPDF_LoadPage(doc, pageIndex);
    if (!page) return results;

    int pageRot = FPDFPage_GetRotation(page);
    double dispW = FPDF_GetPageWidth(page);
    double dispH = FPDF_GetPageHeight(page);
    const QPointF box = pdfBoxOrigin(page);

    FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
    if (!textPage) { FPDF_ClosePage(page); return results; }

    const int nChars = FPDFText_CountChars(textPage);
    if (nChars <= 0) {
        FPDFText_ClosePage(textPage);
        FPDF_ClosePage(page);
        return results;
    }

    std::vector<unsigned short> buf(static_cast<size_t>(nChars) + 1, 0);
    FPDFText_GetText(textPage, 0, nChars, buf.data());
    const QString fullText = QString::fromUtf16(buf.data());

    const FoldMap pageFold = buildFoldMap(fullText);
    const FoldMap queryFold = buildFoldMap(query);
    const QString q = queryFold.folded;
    const int qLen = q.size();

    int pos = 0;
    while (qLen > 0 && (pos = pageFold.folded.indexOf(q, pos, cs)) >= 0) {
        const int end = pos + qLen;
        if (pos >= pageFold.foldedToOrig.size() || end > pageFold.foldedToOrig.size())
            break;
        const int charIdx = pageFold.foldedToOrig.at(pos);
        const int lastOrig = pageFold.foldedToOrig.at(end - 1);
        const int charCount = lastOrig - charIdx + 1;

        QVector<QRectF> rects;
        int n = FPDFText_CountRects(textPage, charIdx, charCount);
        for (int i = 0; i < n; ++i) {
            double left = 0, top = 0, right = 0, bottom = 0;
            if (!FPDFText_GetRect(textPage, i, &left, &top, &right, &bottom))
                continue;
            rects.append(pdfRectToDisp(QRectF(left, bottom, right - left, top - bottom),
                                       dispW, dispH, pageRot, box.x(), box.y()));
        }
        if (rects.isEmpty()) { pos = end; continue; }

        int snippetStart = qMax(0, charIdx - 20);
        int snippetLen = charCount + 40;
        std::vector<unsigned short> sbuf(static_cast<size_t>(snippetLen) + 1, 0);
        FPDFText_GetText(textPage, snippetStart, snippetLen, sbuf.data());
        QString snippet = QString::fromUtf16(sbuf.data());

        results.append({pageIndex, rects, snippet.trimmed(), charIdx, charCount});
        pos = end;
    }

    FPDFText_ClosePage(textPage);
    FPDF_ClosePage(page);
    return results;
}
