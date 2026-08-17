#pragma once
#include <QObject>
#include <QList>
#include <QRectF>
#include <QString>
#include <QVector>
#include "PdfDocument.h"

struct SearchResult {
    int pageIndex;
    QVector<QRectF> rects; // in display coords; ONE rect per text LINE
    QString contextSnippet;
    int charIdx = 0;    // chi so ky tu goc (de doi chieu anh xa khi nghiem thu)
    int charCount = 0;
};

// Searches text in vector PDFs via PDFium FPDFText API.
// For raster pages, delegates to OcrEngine if available.
class TextSearch : public QObject {
    Q_OBJECT
public:
    explicit TextSearch(QObject* parent = nullptr);

    // Start async search. Results emitted via found() signal page by page.
    // matchDiacritics=false (mac dinh): bo dau tieng Viet + theo cs.
    // matchDiacritics=true: khop chinh xac qua FPDFText_FindStart.
    void search(PdfDocument* doc, const QString& query, Qt::CaseSensitivity cs = Qt::CaseInsensitive,
                bool matchDiacritics = false);
    void cancel();

    // Chuan hoa chuoi ve dang so khop: NFD -> bo dau to hop (Mn) -> thuong.
    // Dung chung cho truy van va cho chu trong trang. Tach rieng de nghiem thu.
    static QString foldForMatch(const QString& text);

signals:
    void found(SearchResult result);
    void searchComplete(int totalResults);
    void progress(int pagesScanned, int totalPages);

private:
    QAtomicInt m_cancelled{false};
    QList<SearchResult> searchPageExact(FPDF_DOCUMENT doc, int pageIndex, const QString& query,
                                        Qt::CaseSensitivity cs);
    QList<SearchResult> searchPageFolded(FPDF_DOCUMENT doc, int pageIndex, const QString& query,
                                         Qt::CaseSensitivity cs);
};
