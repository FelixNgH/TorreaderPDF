#pragma once
#include <QObject>
#include <QString>
#include <QList>
#include <QRectF>
#include <QColor>
#include <fpdfview.h>
#include <fpdf_annot.h>

// Flat record for one annotation read from a PDF page.
struct AnnotInfo {
    int     pageIndex = 0;
    QString type;      // "Note", "FreeText", "Highlight", "Underline", etc.
    QString text;      // /Contents
    QString author;    // /T
    QRectF  rect;      // in PDF points, Y upward
    QColor  color;
};

// Reads and creates annotations via PDFium.
// All operations are main-thread only (PDFium is single-threaded for writes).
class AnnotationManager : public QObject {
    Q_OBJECT
public:
    explicit AnnotationManager(QObject* parent = nullptr);

    void setDocument(FPDF_DOCUMENT doc, const QString& filePath);

    // Read all annotations from one page (fast, called per-page).
    QList<AnnotInfo> loadPage(int pageIndex);

    // Read all annotations across the whole document.
    QList<AnnotInfo> loadAll(int pageCount);

    // Create a sticky-note annotation (FPDF_ANNOT_TEXT) at a point on the page.
    // Saves the document to disk.
    bool createPopupNote(int pageIndex, QPointF pointPdf,
                         const QString& text, const QString& author);

    // Create a free-text annotation (FPDF_ANNOT_FREETEXT) over a rect on the page.
    // Saves the document to disk.
    bool createInlineNote(int pageIndex, QRectF rectPdf,
                          const QString& text, const QString& author);

    // Update the Contents string of an existing annotation in place.
    // Saves the document to disk.
    bool updateNote(int pageIndex, int annotIndex, const QString& newText);

    QString lastError() const { return m_lastError; }

signals:
    void annotationAdded(int pageIndex, AnnotInfo info);

private:
    bool saveDocument();

    FPDF_DOCUMENT m_doc     = nullptr;
    QString       m_path;
    QString       m_lastError;
};
