#pragma once
#include <QObject>
#include <QList>
#include <QRectF>
#include <QString>
#include "PdfDocument.h"

struct SearchResult {
    int pageIndex;
    QRectF boundingBox; // in PDF points
    QString contextSnippet;
};

// Searches text in vector PDFs via PDFium FPDFText API.
// For raster pages, delegates to OcrEngine if available.
class TextSearch : public QObject {
    Q_OBJECT
public:
    explicit TextSearch(QObject* parent = nullptr);

    // Start async search. Results emitted via found() signal page by page.
    void search(PdfDocument* doc, const QString& query, Qt::CaseSensitivity cs = Qt::CaseInsensitive);
    void cancel();

signals:
    void found(SearchResult result);
    void searchComplete(int totalResults);
    void progress(int pagesScanned, int totalPages);

private:
    bool m_cancelled = false;
    QList<SearchResult> searchPage(FPDF_DOCUMENT doc, int pageIndex, const QString& query,
                                   Qt::CaseSensitivity cs);
};
