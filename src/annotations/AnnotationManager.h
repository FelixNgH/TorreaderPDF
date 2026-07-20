#pragma once
#include <QObject>
#include <QString>
#include <QList>
#include <QRectF>
#include <QColor>
#include <QVector>
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
    bool isDraft = false;
};

struct AnnotSnapshot {
    bool  valid = false;
    int   subtype = 0;
    float rl = 0, rt = 0, rr = 0, rb = 0;
    unsigned int r = 255, g = 0, b = 0, a = 255;
    bool hasColor = false;
    bool hasFill = false;
    unsigned int fr = 255, fg = 255, fb = 255, fa = 0;
    float border = 2.0f;
    bool isDraft = false;
    QString da;
    QString contents;
    QVector<QVector<QPointF>> ink;
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
                          const QString& text, const QString& author,
                          bool withBackground = true,
                          QColor textColor = Qt::black);

    // Update the Contents string of an existing annotation in place.
    // Saves the document to disk.
    bool updateNote(int pageIndex, int annotIndex, const QString& newText);

    bool removeAnnot(int pageIndex, int index);
    bool setAnnotStyle(int pageIndex, int index, QColor color, float width, bool fill);

    AnnotSnapshot snapshotAnnot(int pageIndex, int index);
    bool addSnapshot(int pageIndex, const AnnotSnapshot& s);

    bool createSignatureDraft(int pageIndex, QRectF rectPt, const QString& text);
    QRectF findSignatureDraftRect(int pageIndex, int* outIndex);

    bool saveDocument();
    QString lastError() const { return m_lastError; }

signals:
    void annotationAdded(int pageIndex, AnnotInfo info);

private:

    FPDF_DOCUMENT m_doc     = nullptr;
    QString       m_path;
    QString       m_lastError;
};
