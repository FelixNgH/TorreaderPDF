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
    int     indexInPage = -1;
    QString type;      // "Note", "FreeText", "Highlight", "Underline", etc.
    QString text;      // /Contents
    QString author;    // /T
    QRectF  rect;      // in PDF points, Y upward
    QColor  color;
    bool isDraft = false;
    QString uid;
};

Q_DECLARE_METATYPE(AnnotInfo)

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
    QString uid;
    QVector<QVector<QPointF>> ink;
};

// Overlay annotation data — coordinates in display space (Y-down, rotation applied).
struct AnnotVisual {
    int      page = 0;
    QString  uid;
    int      subtype = 0;      // FPDF_ANNOT_*
    QRectF   rect;             // display coords (via pdfToDisp)
    QColor   stroke = QColor(Qt::red);
    QColor   fill = QColor(Qt::transparent);
    float    border = 2.0f;
    QVector<QVector<QPointF>> ink;    // INK: strokes, display coords
    QVector<QRectF>           quads;  // HIGHLIGHT: quad points, display coords
    QString  text;
    float    fontSize = 11.0f;
    bool     isNote = false;
    // ponytail: FreeText/Note are drawn as page objects in renderer, not by overlay
    bool     paintByOverlay = true;
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

    // Stream annotations page-by-page, emitting pageAnnotsLoaded per non-empty page.
    void loadAllStreaming(int pageCount, int startPage = 0);

    // Read annotations as overlay visuals for one page.
    QList<AnnotVisual> loadPageVisuals(int page, bool* outOverlayCapable);

    // Create a sticky-note annotation (FPDF_ANNOT_TEXT) at a point on the page.
    // Saves the document to disk.
    bool createPopupNote(int pageIndex, QPointF pointPdf,
                         const QString& text, const QString& author);

    // Create a free-text annotation (FPDF_ANNOT_FREETEXT) over a rect on the page.
    // Saves the document to disk.
    bool createInlineNote(int pageIndex, QRectF rectPdf,
                          const QString& text, const QString& author,
                          bool withBackground = true,
                          QColor textColor = Qt::black,
                          float fontSize = 11.0f);

    // Update the Contents string of an existing annotation in place.
    // Saves the document to disk.
    bool updateNote(int pageIndex, int annotIndex, const QString& newText);

    bool removeAnnot(int pageIndex, int index);
    int removeNotePageObjects(int pageIndex, unsigned int noteId);
    bool setAnnotStyle(int pageIndex, int index, QColor color, float width, bool fill, int fillAlpha = 255);
    bool rebuildTextNote(int pageIndex, int index, QColor newColor, float newFontSize);
    int findAnnotIndexByUid(int pageIndex, const QString& uid);
    QString generateUid();
    bool moveNote(int pageIndex, int index, double dxDisp, double dyDisp);

    AnnotSnapshot snapshotAnnot(int pageIndex, int index);
    bool addSnapshot(int pageIndex, const AnnotSnapshot& s);

    // Read-back for Properties dialog. Returns false if annot does not exist.
    bool getAnnotEditState(int pageIndex, int index, QString& outType,
                           QColor& outColor, float& outWidth, float& outFontSize,
                           bool* outHasFill = nullptr, int* outFillAlpha = nullptr);

    bool createSignatureDraft(int pageIndex, QRectF rectPt, const QString& text);
    QRectF findSignatureDraftRect(int pageIndex, int* outIndex);

    // Generate content for a single page (called just before save from deferred set).
    void generateContentForPage(int page);

    bool saveDocument();
    QString lastError() const { return m_lastError; }
    QString lastCreatedUid() const { return m_lastCreatedUid; }

signals:
    void annotationAdded(int pageIndex, AnnotInfo info);
    void pageAnnotsLoaded(int pageIndex, QList<AnnotInfo> annots);
    void scanProgress(int pagesScanned, int totalPages);

private:

    FPDF_DOCUMENT m_doc     = nullptr;
    QString       m_path;
    QString       m_lastError;
    unsigned int  m_nextNoteId = 1;
    QString m_lastCreatedUid;
};
